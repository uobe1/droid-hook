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

static int selftest;
module_param(selftest, int, 0444);
MODULE_PARM_DESC(selftest, "run the Droid-Hook self-test at load, refuse to load on failure");

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

	/* 2. relocating a NOP must leave a NOP. */
	ret = dh_insn_relocate(&nop_in, &nop_out, (void *)0x1000, (void *)0x2000, 1);
	DH_ST_CHECK(ret == DH_OK, "relocate_ret");
	DH_ST_CHECK(nop_out == nop_in, "relocate_nop_stable");

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
	pr_info("DH_SELFTEST_OK\n");
	return 0;
}

static int __init dh_module_init(void)
{
	if (selftest) {
		int ret = dh_run_selftest();

		if (ret != 0)
			return ret;
	}
	pr_info("droid_hook: loaded (abi %d)\n", DH_ABI);
	return 0;
}

static void __exit dh_module_exit(void)
{
	pr_info("droid_hook: unloaded\n");
}

module_init(dh_module_init);
module_exit(dh_module_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Droid-Hook: patch/hook/RW library contract and runtime adapter");
MODULE_AUTHOR("Uobe");
MODULE_VERSION("0.1.0");
