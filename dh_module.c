/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Droid-Hook kernel module: packaging and self-test only.
 *
 * The library (src/) is deliberately kernel-agnostic: it talks to an injected
 * struct dh_backend.  This file supplies the module boilerplate, exports the
 * public entry points to other modules, and provides a self-test that runs
 * against an in-module memory backend.  The core RW ABI adapter lives in
 * runtime/ and is built separately, because it needs the core headers.
 *
 * selftest=1 runs the checks at load time and refuses to load on failure, so a
 * regression is a load error rather than a silent no-op.  Markers:
 *   DH_SELFTEST_OK
 *   DH_SELFTEST_FAIL:<case>
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "dh.h"

#ifdef DH_WITH_RUNTIME
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include "dh_core_backend.h"
#endif

static int selftest;
module_param(selftest, int, 0444);
MODULE_PARM_DESC(selftest, "run the Droid-Hook self-test at load, refuse to load on failure");

#ifdef DH_WITH_RUNTIME
/*
 * require_core=1: resolve dnsk_core_get via symbol_get (no hard link
 * dependency), require the core API to be ABI 5, and run a real rw check
 * against it.  Holding the API keeps a module reference on dnsk_core until
 * this module unloads; module_exit releases it.
 */
static int require_core;
module_param(require_core, int, 0444);
MODULE_PARM_DESC(require_core, "require dnsk_core ABI 5 and run a real rw self-test");
#endif

/* ---- exported contract ---- */

EXPORT_SYMBOL_GPL(dh_ctx_init);
EXPORT_SYMBOL_GPL(dh_ctx_fini);
EXPORT_SYMBOL_GPL(dh_read);
EXPORT_SYMBOL_GPL(dh_write);
EXPORT_SYMBOL_GPL(dh_xchg64);
EXPORT_SYMBOL_GPL(dh_cas64);
EXPORT_SYMBOL_GPL(dh_probe);
EXPORT_SYMBOL_GPL(dh_txn_begin);
EXPORT_SYMBOL_GPL(dh_txn_record);
EXPORT_SYMBOL_GPL(dh_txn_commit);
EXPORT_SYMBOL_GPL(dh_txn_rollback);
EXPORT_SYMBOL_GPL(dh_insn_gen_absolute_jump);
EXPORT_SYMBOL_GPL(dh_insn_relocate);
EXPORT_SYMBOL_GPL(dh_scan_refs);
EXPORT_SYMBOL_GPL(dh_patch_text_batch);
EXPORT_SYMBOL_GPL(dh_hook_install);
EXPORT_SYMBOL_GPL(dh_hook_uninstall);
EXPORT_SYMBOL_GPL(dh_hook_wrap);
EXPORT_SYMBOL_GPL(dh_shadow_get_or_alloc);
EXPORT_SYMBOL_GPL(dh_shadow_free);
EXPORT_SYMBOL_GPL(dh_preflight);
EXPORT_SYMBOL_GPL(dh_probe_protection);

/* ---- in-module memory backend for the self-test ---- */

#define DH_ST_BYTES 256
static u8 dh_st_mem[DH_ST_BYTES];

static int dh_st_rw(struct dh_rw_req *req)
{
	if (req == NULL || req->addr == NULL)
		return DH_E_INVAL;
	if ((u8 *)req->addr < dh_st_mem ||
	    (u8 *)req->addr + req->len > dh_st_mem + DH_ST_BYTES)
		return DH_E_INVAL;
	switch (req->opcode) {
	case 1: /* READ */
		memcpy(req->dst, req->addr, req->len);
		return DH_OK;
	case 2: /* WRITE */
		memcpy(req->addr, req->src, req->len);
		return DH_OK;
	default:
		return DH_E_OPNOTSUPP;
	}
}

static void *dh_st_probe(const char *name)
{
	return (void *)dh_st_mem;
}

static int dh_st_ro_write(void *addr, const void *src, size_t len,
			  enum dh_route route)
{
	memcpy(addr, src, len);
	return DH_OK;
}

/*
 * dh_ctx_init() requires every backend primitive except alloc/free (see dh.h).
 * The self-test only drives rw/ro_write/probe, so the rest are honest no-ops:
 * dh_text_write/patch_pointer are not exercised here because the in-module
 * backend has no real read-only mapping to patch.
 */
static int dh_st_patch_pointer(void *target, void *value)
{
	(void)target;
	(void)value;
	return DH_E_OPNOTSUPP;
}

static int dh_st_text_write(void *addr, const void *src, size_t len,
			    enum dh_route route)
{
	(void)addr;
	(void)src;
	(void)len;
	(void)route;
	return DH_E_OPNOTSUPP;
}

static void dh_st_flush(void *addr, size_t len)
{
	(void)addr;
	(void)len;
}

static void dh_st_quiesce_begin(void)
{
}

static void dh_st_quiesce_end(void)
{
}

static struct dh_backend dh_st_be = {
	.rw = dh_st_rw,
	.patch_pointer = dh_st_patch_pointer,
	.text_write = dh_st_text_write,
	.ro_write = dh_st_ro_write,
	.probe = dh_st_probe,
	.icache_flush = dh_st_flush,
	.tlb_flush = dh_st_flush,
	.quiesce_begin = dh_st_quiesce_begin,
	.quiesce_end = dh_st_quiesce_end,
	.name = "dh_selftest",
	.abi = DH_ABI,
};

/* ---- self-test ---- */

#define DH_ST_CHECK(cond, tag)						\
	do {								\
		if (!(cond)) {						\
			pr_err("DH_SELFTEST_FAIL:%s\n", tag);		\
			return -EINVAL;					\
		}							\
	} while (0)

#ifdef DH_WITH_RUNTIME
/*
 * ---- runtime split-path self-test --------------------------------------
 *
 * The QEMU gate loads this module with selftest=1, so this is the only place
 * where the core ABI 5 adapter (runtime/dh_core_backend.c) is executed in a
 * real kernel.  A mock dnsk_core_api stands in for the suite's core: it owns a
 * page-aligned buffer, rejects any request over 256 bytes or crossing a page,
 * records the chunk sizes, and can fail the Nth write on purpose.
 *
 * A mock core is not the real core: this proves the adapter's chunking and
 * best-effort restore logic, not the routing of the real core.  README and
 * docs/evidence state that boundary next to the result.
 */
#define DH_ST_RW_MAX 256
#define DH_ST_SPLIT_LEN 600
#define DH_ST_PAGE_OFF 4000	/* first chunk must be 96 bytes */

static u8 dh_st_core_mem[8192] __aligned(4096);

struct dh_st_core_log {
	int calls;
	int writes;
	int fail_on_write;
	int fail_hit;
	int bad_len;
	int bad_page;
	int nr_chunks;
	int chunks[8];
};

static struct dh_st_core_log dh_st_core_log;
static struct dnsk_core_api dh_st_core_api;
static struct dh_core_state dh_st_core_state;
static struct dh_backend dh_st_rt_be;

static int dh_st_core_rw(struct dnsk_rw_req *req)
{
	unsigned long page_off;

	if (req == NULL)
		return DH_E_INVAL;
	if (req->len == 0 || req->len > DH_ST_RW_MAX) {
		dh_st_core_log.bad_len = 1;
		req->ret = DH_E_INVAL;
		return 0;
	}
	if (req->addr == NULL) {
		req->ret = DH_E_INVAL;
		return 0;
	}
	page_off = (unsigned long)req->addr & (PAGE_SIZE - 1);
	if (page_off + req->len > PAGE_SIZE) {
		dh_st_core_log.bad_page = 1;
		req->ret = DH_E_INVAL;
		return 0;
	}

	dh_st_core_log.calls++;
	if (req->opcode == DNSK_RW_READ) {
		if (req->dst == NULL) {
			req->ret = DH_E_INVAL;
			return 0;
		}
		memcpy(req->dst, req->addr, req->len);
		req->ret = 0;
		return 0;
	}
	if (req->opcode == DNSK_RW_WRITE) {
		dh_st_core_log.writes++;
		if (dh_st_core_log.fail_on_write > 0 &&
		    dh_st_core_log.writes == dh_st_core_log.fail_on_write) {
			dh_st_core_log.fail_hit = 1;
			req->ret = -EIO;
			return 0;
		}
		if (req->src == NULL) {
			req->ret = DH_E_INVAL;
			return 0;
		}
		memcpy(req->addr, req->src, req->len);
		if (dh_st_core_log.nr_chunks < 8)
			dh_st_core_log.chunks[dh_st_core_log.nr_chunks++] =
				(int)req->len;
		req->ret = 0;
		return 0;
	}
	req->ret = DH_E_OPNOTSUPP;
	return 0;
}

static void dh_st_core_reset(void)
{
	memset(&dh_st_core_log, 0, sizeof(dh_st_core_log));
}

static void dh_st_fill(u8 *buf, size_t len, u8 seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (u8)(seed + (u8)i);
}

static int dh_run_runtime_selftest(void)
{
	static struct dh_ctx rt_ctx;
	static struct dh_backend rt_test_be;
	static u8 pattern[DH_ST_SPLIT_LEN];
	static u8 back[DH_ST_SPLIT_LEN];
	static u8 snapshot[DH_ST_SPLIT_LEN];
	u8 *low = dh_st_core_mem + DH_ST_PAGE_OFF;
	u8 *base = dh_st_core_mem;
	struct dh_rw_req req;
	int i, ret;

	/* Bind the mock core to the adapter under test. */
	memset(&dh_st_core_api, 0, sizeof(dh_st_core_api));
	dh_st_core_api.magic = DNSK_CORE_MAGIC;
	dh_st_core_api.abi = DNSK_CORE_ABI;
	dh_st_core_api.rw = dh_st_core_rw;
	memset(&dh_st_core_state, 0, sizeof(dh_st_core_state));
	memset(&dh_st_rt_be, 0, sizeof(dh_st_rt_be));
	ret = dh_core_backend_init(&dh_st_rt_be, &dh_st_core_state,
				   &dh_st_core_api);
	DH_ST_CHECK(ret == DH_OK, "runtime_setup");
	ret = dh_core_backend_set_page_size(&dh_st_core_state, PAGE_SIZE);
	DH_ST_CHECK(ret == DH_OK, "runtime_setup");

	/*
	 * The raw adapter supplies no icache_flush/tlb_flush/quiesce primitives
	 * (ABI 5 has none), but dh_ctx_init()/dh_ctx_fini()/dh_txn_rollback()
	 * require all four to be present.  The library tests below therefore
	 * wrap the adapter's memory primitives in a backend whose cache and
	 * quiesce hooks are no-ops; rw/text_write/ro_write/patch_pointer/probe
	 * still execute in the adapter.  A no-op quiesce means the txn test
	 * proves the replay restores content, not that a runtime quiesce window
	 * exists -- README and docs/evidence say exactly that.
	 */
	rt_test_be = dh_st_rt_be;
	rt_test_be.icache_flush = dh_st_flush;
	rt_test_be.tlb_flush = dh_st_flush;
	rt_test_be.quiesce_begin = dh_st_quiesce_begin;
	rt_test_be.quiesce_end = dh_st_quiesce_end;

	/*
	 * T1: 600 B through the library.  dh_write/dh_read already split at
	 * 256 B, so this pins the library's chunking over the adapter's fast
	 * path: every core request <= 256 B and inside one page.
	 */
	memset(dh_st_core_mem, 0, sizeof(dh_st_core_mem));
	dh_st_fill(pattern, DH_ST_SPLIT_LEN, 0x30);
	dh_st_core_reset();
	ret = dh_ctx_init(&rt_ctx, &rt_test_be, "runtime_selftest");
	DH_ST_CHECK(ret == DH_OK, "runtime_setup");
	ret = dh_write(&rt_ctx, low, pattern, DH_ST_SPLIT_LEN);
	DH_ST_CHECK(ret == DH_OK, "runtime_split_chunks");
	DH_ST_CHECK(dh_st_core_log.nr_chunks == 3, "runtime_split_chunks");
	DH_ST_CHECK(!dh_st_core_log.bad_len, "runtime_split_page");
	DH_ST_CHECK(!dh_st_core_log.bad_page, "runtime_split_page");
	memset(back, 0, sizeof(back));
	ret = dh_read(&rt_ctx, low, back, DH_ST_SPLIT_LEN);
	DH_ST_CHECK(ret == DH_OK, "runtime_split_readback");
	DH_ST_CHECK(memcmp(back, pattern, DH_ST_SPLIT_LEN) == 0,
		    "runtime_split_readback");

	/*
	 * T2: one direct 600 B backend request, which is what actually
	 * reaches dh_core_rw_slot's split path (the library never sends more
	 * than 256 B).  At page offset 4000 the only correct split is
	 * 96 / 256 / 248.
	 */
	memset(dh_st_core_mem, 0, sizeof(dh_st_core_mem));
	dh_st_fill(pattern, DH_ST_SPLIT_LEN, 0xa0);
	dh_st_core_reset();
	memset(&req, 0, sizeof(req));
	req.opcode = DNSK_RW_WRITE;
	req.addr = low;
	req.src = pattern;
	req.len = DH_ST_SPLIT_LEN;
	ret = dh_st_rt_be.rw(&req);
	DH_ST_CHECK(ret == DH_OK, "runtime_split_chunks");
	DH_ST_CHECK(dh_st_core_log.nr_chunks == 3, "runtime_split_chunks");
	DH_ST_CHECK(dh_st_core_log.chunks[0] == 96 &&
		    dh_st_core_log.chunks[1] == 256 &&
		    dh_st_core_log.chunks[2] == 248, "runtime_split_chunks");
	DH_ST_CHECK(!dh_st_core_log.bad_len && !dh_st_core_log.bad_page,
		    "runtime_split_page");
	DH_ST_CHECK(memcmp(low, pattern, DH_ST_SPLIT_LEN) == 0,
		    "runtime_split_readback");
	pr_info("DH_SELFTEST_RUNTIME_CHUNKS=%d\n", dh_st_core_log.nr_chunks);

	/*
	 * T3: fail the second write of the same split transfer.  The adapter
	 * must report an error and restore the first chunk it wrote, so the
	 * buffer is byte-identical to the pre-call snapshot.
	 */
	memcpy(snapshot, low, DH_ST_SPLIT_LEN);
	dh_st_fill(pattern, DH_ST_SPLIT_LEN, 0x5a);
	dh_st_core_reset();
	dh_st_core_log.fail_on_write = 2;
	memset(&req, 0, sizeof(req));
	req.opcode = DNSK_RW_WRITE;
	req.addr = low;
	req.src = pattern;
	req.len = DH_ST_SPLIT_LEN;
	ret = dh_st_rt_be.rw(&req);
	DH_ST_CHECK(ret != DH_OK, "runtime_restore");
	DH_ST_CHECK(dh_st_core_log.fail_hit, "runtime_restore");
	DH_ST_CHECK(memcmp(low, snapshot, DH_ST_SPLIT_LEN) == 0,
		    "runtime_restore");

	/*
	 * T4: library atomicity for a failed 600 B write.  Record the three
	 * undo ranges (256/256/88), fail the second chunk of dh_write, then
	 * roll back: the buffer must return byte-for-byte to its original
	 * content.  The rollback goes through the adapter's ro_write path,
	 * which is the sleeping F_RO route.
	 */
	memset(dh_st_core_mem, 0, sizeof(dh_st_core_mem));
	dh_st_fill(pattern, DH_ST_SPLIT_LEN, 0x11);
	memcpy(base, pattern, DH_ST_SPLIT_LEN);
	memcpy(snapshot, base, DH_ST_SPLIT_LEN);
	dh_st_fill(pattern, DH_ST_SPLIT_LEN, 0xc3);
	dh_st_core_reset();
	dh_st_core_log.fail_on_write = 2;
	ret = dh_txn_begin(&rt_ctx);
	DH_ST_CHECK(ret == DH_OK, "runtime_txn_rollback");
	for (i = 0; i < DH_ST_SPLIT_LEN; i += DH_ST_RW_MAX) {
		size_t n = DH_ST_SPLIT_LEN - (size_t)i;

		if (n > DH_ST_RW_MAX)
			n = DH_ST_RW_MAX;
		ret = dh_txn_record(&rt_ctx, base + i, n, DH_ROUTE_ALIAS);
		DH_ST_CHECK(ret == DH_OK, "runtime_txn_rollback");
	}
	ret = dh_write(&rt_ctx, base, pattern, DH_ST_SPLIT_LEN);
	DH_ST_CHECK(ret != DH_OK, "runtime_txn_rollback");
	DH_ST_CHECK(dh_st_core_log.fail_hit, "runtime_txn_rollback");
	ret = dh_txn_rollback(&rt_ctx);
	DH_ST_CHECK(ret == DH_OK, "runtime_txn_rollback");
	DH_ST_CHECK(memcmp(base, snapshot, DH_ST_SPLIT_LEN) == 0,
		    "runtime_txn_rollback");

	ret = dh_ctx_fini(&rt_ctx);
	DH_ST_CHECK(ret == DH_OK, "runtime_setup");
	ret = dh_core_backend_fini(&dh_st_rt_be);
	DH_ST_CHECK(ret == DH_OK, "runtime_setup");

	pr_info("DH_SELFTEST_RUNTIME_OK\n");
	return 0;
}

/* ---- real dnsk_core ABI 5 routing (require_core=1) --------------------- */

static int dh_core_bound;
static struct dnsk_core_api *dh_core_api;
static struct dh_core_state dh_core_state;
static struct dh_backend dh_core_be;
static u64 dh_core_probe_word;

static int dh_core_rw_u64(struct dnsk_core_api *api, u16 opcode, void *addr,
			  u64 *val)
{
	struct dnsk_rw_req req;

	memset(&req, 0, sizeof(req));
	req.opcode = opcode;
	req.addr = addr;
	if (opcode == DNSK_RW_READ)
		req.dst = val;
	else
		req.src = val;
	req.len = sizeof(*val);
	if (api->rw(&req) != 0)
		return DH_E_INVAL;
	return req.ret == 0 ? DH_OK : req.ret;
}

static int dh_core_bind(void)
{
	typedef struct dnsk_core_api *(*core_get_fn)(u16, u16);
	struct dnsk_core_api *api;
	core_get_fn get;
	void *sym;
	void *jiffies;
	u64 j1 = 0, j2 = 0;
	u64 want = 0x1122334455667788ULL;
	u64 got = 0;
	int ret;

	if (!require_core)
		return 0;

	/* symbol_get keeps the build free of a hard dnsk_core_get link
	 * dependency; if core is absent this fails closed instead of loading a
	 * module that silently claims routing works. */
	sym = symbol_get("dnsk_core_get");
	if (sym == NULL) {
		pr_err("DH_SELFTEST_FAIL:core_get_missing\n");
		return -ENOENT;
	}
	get = (core_get_fn)sym;
	api = get(DNSK_CORE_ABI, (u16)sizeof(struct dnsk_core_api));
	symbol_put("dnsk_core_get");
	if (IS_ERR(api)) {
		ret = (int)PTR_ERR(api);
		pr_err("DH_CORE_GET=err:%d\n", ret);
		pr_err("DH_SELFTEST_FAIL:core_get\n");
		return ret;
	}
	pr_info("DH_CORE_GET=ok abi=%u size=%u\n", api->abi,
		(unsigned)sizeof(struct dnsk_core_api));

	ret = dh_core_backend_init(&dh_core_be, &dh_core_state, api);
	if (ret != DH_OK) {
		pr_err("DH_SELFTEST_FAIL:core_bind\n");
		module_put(api->owner);
		return ret;
	}

	jiffies = api->resolve("jiffies");
	if (jiffies == NULL)
		goto fail_rw;
	if (dh_core_rw_u64(api, DNSK_RW_READ, jiffies, &j1) != DH_OK)
		goto fail_rw;
	if (dh_core_rw_u64(api, DNSK_RW_READ, jiffies, &j2) != DH_OK)
		goto fail_rw;
	if (j2 < j1)
		goto fail_rw;
	pr_info("DH_CORE_JIFFIES=%llu,%llu\n", j1, j2);

	/* Write only this module's own u64: a real core rw write, no kernel
	 * global is touched. */
	if (dh_core_rw_u64(api, DNSK_RW_WRITE, &dh_core_probe_word, &want) !=
	    DH_OK)
		goto fail_rw;
	if (dh_core_rw_u64(api, DNSK_RW_READ, &dh_core_probe_word, &got) !=
	    DH_OK)
		goto fail_rw;
	if (got != want)
		goto fail_rw;
	dh_core_probe_word = 0;
	pr_info("DH_CORE_WRITE_READ=ok value=%llu\n", got);

	dh_core_api = api;
	dh_core_bound = 1;
	pr_info("DH_SELFTEST_CORE_OK\n");
	return 0;

fail_rw:
	pr_err("DH_SELFTEST_FAIL:core_rw\n");
	dh_core_backend_fini(&dh_core_be);
	module_put(api->owner);
	return -EIO;
}

static void dh_core_unbind(void)
{
	if (!dh_core_bound)
		return;
	dh_core_backend_fini(&dh_core_be);
	module_put(dh_core_api->owner);
	dh_core_api = NULL;
	dh_core_bound = 0;
}
#endif /* DH_WITH_RUNTIME */

static int dh_run_selftest(void)
{
	/*
	 * struct dh_ctx is ~2.3 KB (8 x 256-byte undo entries) and would blow the
	 * kernel's 2 KB stack-frame limit once dh_run_selftest() inlines into
	 * init_module (clang -Wframe-larger-than).  It is a one-shot load-time
	 * self-test, so a function-local static in module .bss is the right home;
	 * load is serialised, so there is no re-entrancy concern.
	 */
	static struct dh_ctx ctx;
	u32 jump[4];
	u32 nop_in = 0xd503201fu, nop_out = 0;
	u32 b_in, b_out = 0;
	u64 target = 0xffffffc008001000ULL;
	u64 back = 0;
	enum dh_route route = DH_ROUTE_ALIAS;
	int ret;

	/* 1. absolute jump: BR X17 must be word 1 and the literal must carry
	 * the exact target in words 2..3. */
	memset(jump, 0, sizeof(jump));
	ret = dh_insn_gen_absolute_jump(jump, (void *)target);
	DH_ST_CHECK(ret == DH_OK, "gen_jump_ret");
	DH_ST_CHECK(jump[1] == (0xd61f0000u | (17u << 5)), "gen_jump_br_x17");
	DH_ST_CHECK(((u64)jump[2] | ((u64)jump[3] << 32)) == target,
		    "gen_jump_literal");

	/* 2. A supported PC-relative class must be retargeted exactly: a B at
	 * 0x1000 with target 0x1400, relocated to 0x2000, needs imm26 = -0x300.
	 * A NOP is neither PC-relative nor a data-address class, so the same
	 * call must be refused as unsupported, not silently copied -- the host
	 * contract suite pins that (-95, tests/host/test_dh_insn.py).  An
	 * earlier version of this self-test expected DH_OK for the NOP and was
	 * caught by the QEMU gate, never by the host suites. */
	b_in = 0x14000000u | ((0x400u >> 2) & 0x03FFFFFFu);
	ret = dh_insn_relocate(&b_in, &b_out, (void *)0x1000, (void *)0x2000, 1);
	DH_ST_CHECK(ret == DH_OK, "relocate_b_ret");
	DH_ST_CHECK(b_out == 0x17FFFD00u, "relocate_b_encoding");
	ret = dh_insn_relocate(&nop_in, &nop_out, (void *)0x1000, (void *)0x2000, 1);
	DH_ST_CHECK(ret == DH_E_OPNOTSUPP, "relocate_nop_rejected");

	/* 3. a transaction over the in-module backend must read back what it
	 * wrote and leave the undo log clean after commit. */
	memset(dh_st_mem, 0, sizeof(dh_st_mem));
	memset(&ctx, 0, sizeof(ctx));
	ret = dh_ctx_init(&ctx, &dh_st_be, "selftest");
	DH_ST_CHECK(ret == DH_OK, "ctx_init");
	DH_ST_CHECK(dh_preflight(&ctx) == DH_OK, "preflight");

	/* 3a. the protection probe performs a same-byte write and must report
	 * the route it used, without changing the probed word. */
	ret = dh_probe_protection(&ctx, dh_st_mem + 16, &route);
	DH_ST_CHECK(ret == DH_OK, "probe_protection");
	DH_ST_CHECK(route == DH_ROUTE_FIXMAP, "probe_route_fixmap");

	ret = dh_txn_begin(&ctx);
	DH_ST_CHECK(ret == DH_OK, "txn_begin");
	ret = dh_txn_record(&ctx, dh_st_mem + 8, sizeof(u64), DH_ROUTE_FIXMAP);
	DH_ST_CHECK(ret == DH_OK, "txn_record");
	ret = dh_write(&ctx, dh_st_mem + 8, &target, sizeof(target));
	DH_ST_CHECK(ret == DH_OK, "write");
	ret = dh_read(&ctx, dh_st_mem + 8, &back, sizeof(back));
	DH_ST_CHECK(ret == DH_OK, "read");
	DH_ST_CHECK(back == target, "read_back_matches");
	ret = dh_txn_commit(&ctx);
	DH_ST_CHECK(ret == DH_OK, "txn_commit");

	/* 4. a refused write must roll the transaction back. */
	ret = dh_txn_begin(&ctx);
	DH_ST_CHECK(ret == DH_OK, "txn2_begin");
	ret = dh_txn_rollback(&ctx);
	DH_ST_CHECK(ret == DH_OK, "txn2_rollback");

	dh_ctx_fini(&ctx);

#ifdef DH_WITH_RUNTIME
	ret = dh_run_runtime_selftest();
	if (ret != 0)
		return ret;
#endif

	pr_info("DH_SELFTEST_OK\n");
	return 0;
}

static int __init dh_module_init(void)
{
	int ret;

	if (selftest) {
		ret = dh_run_selftest();
		if (ret != 0)
			return ret;
	}
#ifdef DH_WITH_RUNTIME
	ret = dh_core_bind();
	if (ret != 0)
		return ret;
#endif
	pr_info("droid_hook: loaded (abi %d)\n", DH_ABI);
	return 0;
}

static void __exit dh_module_exit(void)
{
#ifdef DH_WITH_RUNTIME
	dh_core_unbind();
#endif
	pr_info("droid_hook: unloaded\n");
}

module_init(dh_module_init);
module_exit(dh_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Droid-Hook: patch/hook/RW library contract and runtime adapter");
MODULE_AUTHOR("Uobe");
MODULE_VERSION("0.1.0");
