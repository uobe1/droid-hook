/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host harness for dh_preflight() and dh_probe_protection().
 *
 * Drives the real implementation against an in-process memory backend and
 * prints key=value lines for the Python assertions.  Faults are injected with
 * flags so a wrong verdict is observable rather than assumed.
 *
 * The backend must satisfy dh_ctx_init()'s mandatory-primitive contract (every
 * function pointer except alloc/free; see dh.h).  An earlier draft only filled
 * rw/probe/ro_write, so dh_ctx_init() rejected it with DH_E_INVAL and every
 * probe/preflight call downstream reported -22 instead of the behaviour under
 * test.  The extra primitives below are no-ops: this harness only exercises the
 * preflight/probe paths, which use rw and ro_write.
 */

#include "dh.h"

#include <stdio.h>
#include <string.h>

#define MEM_BYTES 512
static unsigned char mem[MEM_BYTES];

static int ro_write_disabled;
static int ro_write_corrupts;
static int rw_read_fail_ret;

static int be_rw(struct dh_rw_req *req)
{
	if (req == NULL || req->addr == NULL)
		return DH_E_INVAL;
	unsigned char *p = (unsigned char *)req->addr;

	if (p < mem || p + req->len > mem + MEM_BYTES)
		return DH_E_INVAL;
	if (req->opcode == 1) {          /* READ */
		if (rw_read_fail_ret)
			return rw_read_fail_ret;    /* inject a real read failure */
		memcpy(req->dst, req->addr, req->len);
		return DH_OK;
	}
	if (req->opcode == 2) {          /* WRITE */
		memcpy(req->addr, req->src, req->len);
		return DH_OK;
	}
	return DH_E_OPNOTSUPP;
}

static void *be_probe(const char *name)
{
	(void)name;
	return mem;
}

static int be_ro_write(void *addr, const void *src, size_t len, enum dh_route route)
{
	(void)route;
	if (ro_write_disabled)
		return DH_E_OPNOTSUPP;
	memcpy(addr, src, len);
	if (ro_write_corrupts)
		((unsigned char *)addr)[0] ^= 0xff;
	return DH_OK;
}

/* ---- mandatory no-op primitives (see dh.h contract) ---- */

static int be_patch_pointer(void *target, void *value)
{
	(void)target;
	(void)value;
	return DH_OK;
}

static int be_text_write(void *addr, const void *src, size_t len,
			 enum dh_route route)
{
	(void)addr;
	(void)src;
	(void)len;
	(void)route;
	return DH_OK;
}

static void be_icache_flush(void *addr, size_t len)
{
	(void)addr;
	(void)len;
}

static void be_tlb_flush(void *addr, size_t len)
{
	(void)addr;
	(void)len;
}

static void be_quiesce_begin(void) { }
static void be_quiesce_end(void) { }

static struct dh_backend be = {
	.rw = be_rw,
	.patch_pointer = be_patch_pointer,
	.text_write = be_text_write,
	.ro_write = be_ro_write,
	.probe = be_probe,
	.icache_flush = be_icache_flush,
	.tlb_flush = be_tlb_flush,
	.quiesce_begin = be_quiesce_begin,
	.quiesce_end = be_quiesce_end,
	.name = "preflight_harness",
	.abi = DH_ABI,
};

int main(int argc, char **argv)
{
	struct dh_ctx ctx;
	enum dh_route route = (enum dh_route)0;
	int ret;
	unsigned long long word = 0x1122334455667788ULL;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <cmd>\n", argv[0]);
		return 2;
	}
	memset(mem, 0, sizeof(mem));
	memcpy(mem + 64, &word, sizeof(word));

	memset(&ctx, 0, sizeof(ctx));
	printf("init=%d\n", dh_ctx_init(&ctx, &be, "harness"));

	if (strcmp(argv[1], "preflight_ok") == 0) {
		printf("preflight=%d\n", dh_preflight(&ctx));
	} else if (strcmp(argv[1], "preflight_busy") == 0) {
		printf("begin=%d\n", dh_txn_begin(&ctx));
		printf("preflight=%d\n", dh_preflight(&ctx));
	} else if (strcmp(argv[1], "preflight_bad_abi") == 0) {
		be.abi = (u16)(DH_ABI + 7);
		printf("preflight=%d\n", dh_preflight(&ctx));
	} else if (strcmp(argv[1], "preflight_null") == 0) {
		printf("preflight_null_ctx=%d\n", dh_preflight(NULL));
	} else if (strcmp(argv[1], "preflight_uninit") == 0) {
		struct dh_ctx zeroed;

		memset(&zeroed, 0, sizeof(zeroed));
		zeroed.be = &be;    /* valid backend, but initialized == 0 */
		printf("preflight_uninit=%d\n", dh_preflight(&zeroed));
	} else if (strcmp(argv[1], "probe_ok") == 0) {
		ret = dh_probe_protection(&ctx, mem + 64, &route);
		printf("probe=%d\n", ret);
		printf("route=%d\n", (int)route);
		printf("bytes_kept=%d\n", memcmp(mem + 64, &word, sizeof(word)) == 0);
	} else if (strcmp(argv[1], "probe_no_ro") == 0) {
		ro_write_disabled = 1;
		printf("probe=%d\n", dh_probe_protection(&ctx, mem + 64, &route));
	} else if (strcmp(argv[1], "probe_corrupt") == 0) {
		ro_write_corrupts = 1;
		printf("probe=%d\n", dh_probe_protection(&ctx, mem + 64, &route));
	} else if (strcmp(argv[1], "probe_read_fail") == 0) {
		rw_read_fail_ret = DH_E_NOENT;
		printf("probe=%d\n", dh_probe_protection(&ctx, mem + 64, &route));
	} else if (strcmp(argv[1], "probe_null_addr") == 0) {
		printf("probe=%d\n", dh_probe_protection(&ctx, NULL, &route));
	} else {
		fprintf(stderr, "unknown cmd\n");
		return 2;
	}
	return 0;
}
