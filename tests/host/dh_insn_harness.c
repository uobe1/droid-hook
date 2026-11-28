/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Host harness for droid-hook/src/dh_insn.c.
 *
 * Compiled and linked together with the real src/dh_insn.c by
 * test_dh_insn.py.  It supplies the cross-TU stub (dh_txn_rollback), a fake
 * backend, and a synthetic kernel-text window, then prints machine-readable
 * key=value lines for the Python assertions.
 *
 * It deliberately does NOT implement any encoder logic of its own: every
 * encoding under test comes from the library translation unit.
 */

#include "dh.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Same dh.h host-contract gap as dh_insn.c: no s64 on the host side. */
typedef int64_t s64;

#ifdef DH_INS_TEST_SEAM
/*
 * Test-only wrapper exported by dh_insn.c when it is compiled with
 * -DDH_INS_TEST_SEAM (test_dh_insn.py does that).  It calls the same static
 * pair re-encoder the production scanner uses, so these assertions cover
 * production code.  A production build never defines the macro.
 */
int dh_ins_test_pair_retarget(u32 adrp, u32 follow, u64 adrp_pc,
			      u64 old_base, u64 new_base,
			      u32 *out_adrp, u32 *out_follow);
#endif

/* ---- cross-TU stubs owned by dh_txn.c / dh_rw.c in a real build ---- */

static int g_rollback_calls;

int dh_txn_begin(struct dh_ctx *ctx)
{
	if (!ctx)
		return DH_E_INVAL;
	ctx->txn.active = 1;
	ctx->txn.n = 0;
	ctx->txn.failed = 0;
	return DH_OK;
}

int dh_txn_record(struct dh_ctx *ctx, void *addr, size_t len,
		  enum dh_route route)
{
	struct dh_undo_ent *e;

	if (!ctx || !ctx->txn.active || !addr || len == 0 || len > 256)
		return DH_E_INVAL;
	if (ctx->txn.n >= DH_UNDO_SLOTS)
		return DH_E_NOSPC;
	e = &ctx->txn.undo[ctx->txn.n++];
	e->addr = addr;
	e->len = len;
	e->route = route;
	e->used = 1;
	memcpy(e->orig, addr, len);
	return DH_OK;
}

int dh_txn_commit(struct dh_ctx *ctx)
{
	if (!ctx || !ctx->txn.active)
		return DH_E_INVAL;
	ctx->txn.active = 0;
	ctx->txn.n = 0;
	return DH_OK;
}

int dh_txn_rollback(struct dh_ctx *ctx)
{
	u32 i;

	if (!ctx)
		return DH_E_INVAL;
	g_rollback_calls++;
	for (i = ctx->txn.n; i > 0; i--) {
		struct dh_undo_ent *e = &ctx->txn.undo[i - 1];

		if (e->used && e->addr)
			memcpy(e->addr, e->orig, e->len);
	}
	ctx->txn.active = 0;
	ctx->txn.n = 0;
	ctx->txn.failed = 1;
	return DH_OK;
}

int dh_read(struct dh_ctx *ctx, void *addr, void *dst, size_t len)
{
	if (!ctx || !addr || !dst || !len)
		return DH_E_INVAL;
	memcpy(dst, addr, len);
	return DH_OK;
}

/* ---- fake backend ---- */

#define TEXT_WORDS 64
static u32 g_text[TEXT_WORDS];
static void *g_probe_stext;
static void *g_probe_etext;
static int g_text_writes;
static int g_fail_at = -1;
static int g_text_lie;

static void *fake_probe(const char *name)
{
	if (strcmp(name, "_stext") == 0)
		return g_probe_stext;
	if (strcmp(name, "_etext") == 0)
		return g_probe_etext;
	return NULL;
}

static int fake_text_write(void *addr, const void *src, size_t len,
			   enum dh_route route)
{
	(void)route;
	if (g_fail_at >= 0 && g_text_writes == g_fail_at)
		return DH_E_VERIFY;
	if (g_text_lie) {
		/* Report success without writing, so the per-site read-back in
		 * dh_patch_text_batch() must catch it. */
		g_text_writes++;
		return DH_OK;
	}
	if (len)
		memcpy(addr, src, len);
	g_text_writes++;
	return DH_OK;
}

static struct dh_backend g_be = {
	.text_write = fake_text_write,
	.probe = fake_probe,
	.name = "host-harness",
	.abi = DH_ABI,
};

static struct dh_ctx g_ctx;

/* ---- synthetic instruction builders (independent of dh_insn.c) ---- */

static u32 mk_adrp(s64 page_delta, u32 rd)
{
	u32 lo = (u32)((u64)page_delta & 3);
	u32 hi = (u32)(((u64)page_delta >> 2) & 0x7FFFF);

	return 0x90000000u | (lo << 29) | (hi << 5) | rd;
}

static u32 mk_add(u32 imm12, u32 sh, u32 rn, u32 rd)
{
	return 0x91000000u | (sh << 22) | (imm12 << 10) | (rn << 5) | rd;
}

static u32 mk_ldr_x(u32 imm12_scaled, u32 rn, u32 rt)
{
	/* 0xF9400000 = LDR x (unsigned offset), opc = 0b11 in bits[23:22] */
	return 0xF9400000u | (imm12_scaled << 10) | (rn << 5) | rt;
}

static u32 mk_ldr_lit_x(s64 word_delta, u32 rt)
{
	return 0x58000000u | (((u32)word_delta & 0x7FFFF) << 5) | rt;
}

/* LDRB (unsigned offset): size=0b00 so imm12 is scaled by 1. */
static u32 mk_ldrb(u32 imm12, u32 rn, u32 rt)
{
	return 0x39400000u | (imm12 << 10) | (rn << 5) | rt;
}

/* SUB (immediate), 64-bit: bit[30] set. */
static u32 mk_sub(u32 imm12, u32 sh, u32 rn, u32 rd)
{
	return 0x51000000u | (sh << 22) | (imm12 << 10) | (rn << 5) | rd;
}

/* ADDS / SUBS (immediate), 64-bit: bit[30]=op, bit[29]=S. */
static u32 mk_adds(u32 imm12, u32 sh, u32 rn, u32 rd)
{
	return 0xB1000000u | (sh << 22) | (imm12 << 10) | (rn << 5) | rd;
}

static u32 mk_subs(u32 imm12, u32 sh, u32 rn, u32 rd)
{
	return 0xF1000000u | (sh << 22) | (imm12 << 10) | (rn << 5) | rd;
}

/* Sign-extend an n-bit field already shifted down to bit 0. */
static s64 sext_field(u32 field, unsigned int bits)
{
	s64 v = (s64)field;
	s64 sign = (s64)1 << (bits - 1);

	return (v & (sign - 1)) - (v & sign);
}

static void emit(const char *sub)
{
	memset(g_text, 0, sizeof(g_text));
	g_probe_stext = (void *)(uintptr_t)g_text;
	g_probe_etext = (void *)(uintptr_t)(g_text + TEXT_WORDS);
	g_rollback_calls = 0;
	g_text_writes = 0;
	g_fail_at = -1;
	g_text_lie = 0;
	memset(&g_ctx, 0, sizeof(g_ctx));
	g_ctx.be = &g_be;
	g_ctx.initialized = 1;
	(void)sub;
}

static void print_u32(const char *key, u32 v)
{
	printf("%s=0x%08" PRIx32 "\n", key, v);
}

static void print_int(const char *key, int v)
{
	printf("%s=%d\n", key, v);
}

static void print_u64(const char *key, u64 v)
{
	printf("%s=0x%016" PRIx64 "\n", key, v);
}

/* Decode an ADRP the way the reference prototype does, for verification. */
static s64 decode_adrp_pages(u32 insn)
{
	s64 imm = (s64)((((insn >> 5) & 0x7FFFF) << 2) | ((insn >> 29) & 3));

	if (imm & (1 << 20))
		imm |= ~((1 << 21) - 1);
	return imm;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "";

	if (strcmp(cmd, "jump") == 0) {
		u32 out[4];
		u64 target = 0x1122334455667788ULL;
		int ret = dh_insn_gen_absolute_jump(out, (void *)(uintptr_t)target);

		print_int("ret", ret);
		print_u32("w0", out[0]);
		print_u32("w1", out[1]);
		print_u32("w2", out[2]);
		print_u32("w3", out[3]);
		print_int("ret_null", dh_insn_gen_absolute_jump(NULL, NULL));
		return 0;
	}

	if (strcmp(cmd, "reloc_adrp") == 0) {
		/* ADRP at 0x40000000 pointing +2 pages and -3 pages. */
		u32 src_pos = mk_adrp(2, 0);
		u32 src_neg = mk_adrp(-3, 1);
		u32 dst_pos, dst_neg;
		u64 spc = 0x40000000ULL;
		u64 dpc = 0x40001000ULL; /* +1 page => delta shrinks by 1 */
		int r1 = dh_insn_relocate(&src_pos, &dst_pos,
					  (void *)(uintptr_t)spc,
					  (void *)(uintptr_t)dpc, 1);
		int r2 = dh_insn_relocate(&src_neg, &dst_neg,
					  (void *)(uintptr_t)spc,
					  (void *)(uintptr_t)dpc, 1);

		print_int("r_pos", r1);
		print_int("r_neg", r2);
		print_u32("src_pos", src_pos);
		print_u32("dst_pos", dst_pos);
		print_u32("src_neg", src_neg);
		print_u32("dst_neg", dst_neg);
		print_int("pages_pos", (int)decode_adrp_pages(dst_pos));
		print_int("pages_neg", (int)decode_adrp_pages(dst_neg));
		/* Target page must be unchanged: +2 pages from 0x40000000 =
		 * 0x40002000; from new PC 0x40001000 that is +1 page. */
		print_u64("target_from_new_pc",
			  (dpc & ~0xFFFULL) +
			  ((u64)decode_adrp_pages(dst_pos) << 12));
		return 0;
	}

	if (strcmp(cmd, "add_sh") == 0) {
		/* ADD x2,x2,#0x123,lsl#12 (sh=1) and ADD x2,x2,#0x123 (sh=0). */
		u32 src_sh1 = 0x91448C42u;
		u32 src_sh0 = 0x91048C42u;
		u32 dst_sh1, dst_sh0;
		int r1 = dh_insn_relocate(&src_sh1, &dst_sh1, NULL, NULL, 1);
		int r2 = dh_insn_relocate(&src_sh0, &dst_sh0, NULL, NULL, 1);

		print_int("r_sh1", r1);
		print_int("r_sh0", r2);
		print_u32("dst_sh1", dst_sh1);
		print_u32("dst_sh0", dst_sh0);
		print_u32("sh_bit_sh1", (dst_sh1 >> 22) & 1);
		print_u32("sh_bit_sh0", (dst_sh0 >> 22) & 1);
		print_u32("imm12_sh1", (dst_sh1 >> 10) & 0xFFF);
		return 0;
	}

	if (strcmp(cmd, "ldr_opc") == 0) {
		/* LDR x4,[x5,#64] = 0xF94020A4; opc bits[23:22] must stay 0b11.
		 * LDRB w6,[x7,#1] = 0x394004E6; size/opc must stay. */
		u32 src_ldrx = 0xF94020A4u;
		u32 src_ldrb = 0x394004E6u;
		u32 dst_ldrx, dst_ldrb;
		int r1 = dh_insn_relocate(&src_ldrx, &dst_ldrx, NULL, NULL, 1);
		int r2 = dh_insn_relocate(&src_ldrb, &dst_ldrb, NULL, NULL, 1);

		print_int("r_ldrx", r1);
		print_int("r_ldrb", r2);
		print_u32("dst_ldrx", dst_ldrx);
		print_u32("dst_ldrb", dst_ldrb);
		print_u32("opc_ldrx", (dst_ldrx >> 22) & 3);
		print_u32("opc_ldrb", (dst_ldrb >> 22) & 3);
		print_u32("imm12_ldrx", (dst_ldrx >> 10) & 0xFFF);
		return 0;
	}

	if (strcmp(cmd, "ldr_lit") == 0) {
		/* LDR x9, #0x100 from 0x1000 = word delta 64, fits ±1 MiB.
		 * Moving the instruction +0x1000 must subtract 0x400 words. */
		u32 src = mk_ldr_lit_x(64, 9);
		u32 dst;
		u64 spc = 0x1000ULL, dpc = 0x2000ULL;
		int r = dh_insn_relocate(&src, &dst, (void *)(uintptr_t)spc,
					 (void *)(uintptr_t)dpc, 1);
		s64 new_imm = (s64)((dst >> 5) & 0x7FFFF);

		if (new_imm & (1 << 18))
			new_imm |= ~((1 << 19) - 1);
		print_int("r", r);
		print_u32("src", src);
		print_u32("dst", dst);
		print_int("word_delta", (int)new_imm);
		print_u64("target", (dpc + (u64)new_imm * 4));
		return 0;
	}

	if (strcmp(cmd, "range") == 0) {
		/* ADRP ±4 GiB: put the target 4 GiB away, must be rejected.
		 * B ±128 MiB: target 128 MiB away, rejected.
		 * LDR literal ±1 MiB: target 1 MiB away, rejected. */
		u32 adrp = mk_adrp(2, 0);
		u32 dst;
		u64 spc = 0x40000000ULL;
		u64 far = spc + (1ULL << 33); /* 8 GiB away: page delta > 2^20 */
		int r_adrp = dh_insn_relocate(&adrp, &dst,
					      (void *)(uintptr_t)spc,
					      (void *)(uintptr_t)far, 1);
		u32 b = 0x14000000u | 0x1000u; /* B +#0x4000 */
		int r_b = dh_insn_relocate(&b, &dst,
					   (void *)(uintptr_t)spc,
					   (void *)(uintptr_t)(spc + (2ULL << 27)), 1);
		u32 lit = mk_ldr_lit_x(1, 9);
		int r_lit = dh_insn_relocate(&lit, &dst,
					     (void *)(uintptr_t)spc,
					     (void *)(uintptr_t)(spc + (2ULL << 20)), 1);
		/* A supported instruction with a small move must succeed. */
		int r_ok = dh_insn_relocate(&adrp, &dst,
					    (void *)(uintptr_t)spc,
					    (void *)(uintptr_t)(spc + 0x1000), 1);

		print_int("r_adrp_out", r_adrp);
		print_int("r_b_out", r_b);
		print_int("r_lit_out", r_lit);
		print_int("r_ok", r_ok);
		print_int("opnotsupp", DH_E_OPNOTSUPP);
		return 0;
	}

	if (strcmp(cmd, "scan") == 0) {
		/* The scan reads real host memory through the probe window, so
		 * derive the fake old-array address from g_text's real address:
		 * old = page(g_text) + 16 pages, length 64 bytes.  That keeps the
		 * ADRP page delta well inside the 21-bit signed range.
		 *
		 * Pair 0: ADRP(old page) + ADD #0x20 -> old+0x20  (IN)
		 * Pair 1: ADRP(old page) + LDR #4    -> old+0x20  (IN)
		 * Pair 2: ADRP(old page +1) + ADD #0x10 -> old+0x1010 (OUT)
		 * Others: NOPs and a bare ADRP with no follower.
		 */
		u64 text_page = (u64)(uintptr_t)g_text & ~0xFFFULL;
		u64 old = text_page + (16ULL << 12);
		s64 dp = (s64)((old - text_page) >> 12);
		void *sites[8];
		int n;

		emit("scan");
		g_text[0] = mk_adrp(dp, 0);
		g_text[1] = mk_add(0x20, 0, 0, 0);
		g_text[2] = mk_adrp(dp, 2);
		g_text[3] = mk_ldr_x(4, 2, 3); /* old+0x20, inside the 64 B array */
		g_text[4] = mk_adrp(dp + 1, 4);
		g_text[5] = mk_add(0x10, 0, 4, 4);
		g_text[6] = 0xD503201Fu; /* NOP */
		g_text[7] = mk_adrp(dp, 6);
		g_text[8] = 0xD503201Fu; /* bare ADRP, no follower pair */
		g_probe_stext = (void *)(uintptr_t)g_text;
		g_probe_etext = (void *)(uintptr_t)(g_text + 16);
		n = dh_scan_refs(&g_ctx, (void *)(uintptr_t)old, 64,
				 (void *)(uintptr_t)(old + 0x1000), sites, 8);
		print_int("n", n);
		print_u64("site0", (u64)(uintptr_t)sites[0]);
		print_u64("site1", (u64)(uintptr_t)sites[1]);
		print_u64("expect0", (u64)(uintptr_t)&g_text[0]);
		print_u64("expect1", (u64)(uintptr_t)&g_text[2]);
		print_u64("old", old);
		return 0;
	}

	if (strcmp(cmd, "scan_empty") == 0) {
		void *sites[4];
		int n;

		emit("scan_empty");
		g_probe_stext = (void *)(uintptr_t)g_text;
		g_probe_etext = (void *)(uintptr_t)(g_text + 16);
		memset(g_text, 0xD5, sizeof(g_text)); /* all NOP-ish */
		n = dh_scan_refs(&g_ctx, (void *)(uintptr_t)0x40010000ULL, 64,
				 NULL, sites, 4);
		print_int("n", n);
		/* Bad arguments must be rejected, not reported as "0 sites". */
		print_int("r_null", dh_scan_refs(&g_ctx, NULL, 64, NULL, sites, 4));
		return 0;
	}

	if (strcmp(cmd, "batch") == 0) {
		void *addrs[3];
		u32 buf0[2] = { 0x11111111u, 0x22222222u };
		u32 buf1[2] = { 0x33333333u, 0x44444444u };
		u32 buf2[2] = { 0x55555555u, 0x66666666u };
		u32 *insns[3] = { buf0, buf1, buf2 };
		size_t lens[3] = { 8, 8, 8 };
		int r;

		emit("batch");
		addrs[0] = &g_text[0];
		addrs[1] = &g_text[2];
		addrs[2] = &g_text[4];
		r = dh_patch_text_batch(&g_ctx, addrs, insns, lens, 3);
		print_int("r_ok", r);
		print_int("writes", g_text_writes);
		print_u32("w0", g_text[0]);
		print_u32("w1", g_text[1]);

		/* Failure at the second write must roll back exactly once. */
		emit("batch");
		g_fail_at = 1;
		addrs[0] = &g_text[0];
		addrs[1] = &g_text[2];
		addrs[2] = &g_text[4];
		r = dh_patch_text_batch(&g_ctx, addrs, insns, lens, 3);
		print_int("r_fail", r);
		print_int("rollback_calls", g_rollback_calls);
		print_int("writes_on_fail", g_text_writes);
		/* Site 0 was written before site 1 failed; its undo entry must
		 * have restored it to the emit() pattern (0x1000, 0x1001). */
		print_u32("w0_after_fail", g_text[0]);
		print_u32("w1_after_fail", g_text[1]);
		print_int("txn_active_after_fail", g_ctx.txn.active);
		print_int("txn_n_after_fail", (int)g_ctx.txn.n);
		return 0;
	}

	if (strcmp(cmd, "batch_verify") == 0) {
		/* text_write() reports success but leaves the text untouched.
		 * The per-site read-back must turn that into DH_E_VERIFY, not a
		 * silent success. */
		void *addrs[1];
		u32 buf[2] = { 0xDEADBEEFu, 0xFEEDFACEu };
		u32 *insns[1] = { buf };
		size_t lens[1] = { 8 };
		int r;

		emit("batch_verify");
		addrs[0] = &g_text[0];
		g_text_lie = 1;
		r = dh_patch_text_batch(&g_ctx, addrs, insns, lens, 1);
		g_text_lie = 0;
		print_int("r", r);
		print_int("verify", DH_E_VERIFY);
		print_int("writes", g_text_writes);
		print_int("rollback_calls", g_rollback_calls);
		print_u32("w0", g_text[0]);
		return 0;
	}

	if (strcmp(cmd, "batch_in_txn") == 0) {
		/* A batch must reject a caller-owned open transaction instead of
		 * recording into it or rolling it back. */
		void *addrs[1];
		u32 buf[2] = { 0xABCDEF01u, 0x12345678u };
		u32 *insns[1] = { buf };
		size_t lens[1] = { 8 };
		int r, rb;

		emit("batch_in_txn");
		addrs[0] = &g_text[0];
		rb = dh_txn_begin(&g_ctx);
		r = dh_patch_text_batch(&g_ctx, addrs, insns, lens, 1);
		print_int("begin_ret", rb);
		print_int("r", r);
		print_int("writes", g_text_writes);
		print_int("rollback_calls", g_rollback_calls);
		print_int("txn_active", g_ctx.txn.active);
		print_int("busy", DH_E_BUSY);
		return 0;
	}

	if (strcmp(cmd, "batch_cap") == 0) {
		void *addrs[DH_MAX_BATCH + 1];
		u32 one = 0xDEADBEEFu;
		u32 *insns[DH_MAX_BATCH + 1];
		size_t lens[DH_MAX_BATCH + 1];
		size_t i;
		int r;

		emit("batch_cap");
		for (i = 0; i < DH_MAX_BATCH + 1; i++) {
			addrs[i] = &g_text[0];
			insns[i] = &one;
			lens[i] = 4;
		}
		r = dh_patch_text_batch(&g_ctx, addrs, insns, lens,
					DH_MAX_BATCH + 1);
		print_int("r_cap", r);
		print_int("nospc", DH_E_NOSPC);
		print_int("writes", g_text_writes);
		return 0;
	}

	if (strcmp(cmd, "reloc_bad") == 0) {
		/* Bad arguments are errors, not silent success.  A literal form
		 * the relocator cannot encode is refused (copying it would point
		 * the trampoline at the wrong data); a position-independent word
		 * such as MOVZ is copied verbatim. */
		u32 far = 0x14000000u;	/* b +0, moved 1 TiB away: unencodable */
		u32 movz = 0xD2800000u;	/* movz x0, #0 */
		u32 prfm = 0xD8000000u;	/* prfm (literal), imm19 = 0 */
		u32 d = 0, kept = 0, prfm_dst = 0;

		print_int("r_null_src", dh_insn_relocate(NULL, &d, NULL, NULL, 1));
		print_int("r_null_dst", dh_insn_relocate(&far, NULL, NULL, NULL, 1));
		print_int("r_nr0", dh_insn_relocate(&far, &d, NULL, NULL, 0));
		print_int("r_unknown",
			  dh_insn_relocate(&far, &d, NULL,
					   (void *)(uintptr_t)(1ULL << 40), 1));
		print_int("r_verbatim",
			  dh_insn_relocate(&movz, &kept, NULL, NULL, 1));
		print_u32("verbatim_dst", kept);
		print_int("r_prfm",
			  dh_insn_relocate(&prfm, &prfm_dst, (void *)0x1000,
					   (void *)0x2000, 1));
		print_u32("prfm_dst", prfm_dst);
		print_int("inval", DH_E_INVAL);
		print_int("opnotsupp", DH_E_OPNOTSUPP);
		return 0;
	}

	if (strcmp(cmd, "range_boundary") == 0) {
		/* ADRP's 21-bit page delta is [-2^20, 2^20-1] pages.  Pin both
		 * edges: 2^20-1 and -2^20 must encode, 2^20 and -2^20-1 must be
		 * refused instead of wrapped. */
		const u64 p0 = 0x4000000000ULL;
		static const s64 pages[] = { 0xFFFFF, 0x100000,
					    -0x100000, -0x100001 };
		static const char *const names[] = { "max", "over", "min",
						    "under" };
		u32 adrp = mk_adrp(0, 0);
		size_t i;

		for (i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
			u32 out = 0;
			u64 dpc = (u64)((s64)p0 - pages[i] * 4096);
			int r = dh_insn_relocate(&adrp, &out,
						 (void *)(uintptr_t)p0,
						 (void *)(uintptr_t)dpc, 1);

			printf("bound_%s_ret=%d\n", names[i], r);
			printf("bound_%s_pages=%d\n", names[i],
			       (int)decode_adrp_pages(out));
		}
		return 0;
	}

	if (strcmp(cmd, "ldr_lit_neg") == 0) {
		/* Backward literal reference: -64 words from 0x2000 targets
		 * 0x1F00; moving to 0x1000 must re-encode to +960 words. */
		u32 src = mk_ldr_lit_x(-64, 9);
		u32 dst = 0;
		u64 spc = 0x2000ULL, dpc = 0x1000ULL;
		int r = dh_insn_relocate(&src, &dst, (void *)(uintptr_t)spc,
					 (void *)(uintptr_t)dpc, 1);
		s64 imm = (s64)((dst >> 5) & 0x7FFFF);

		if (imm & (1 << 18))
			imm |= ~((1 << 19) - 1);
		print_int("r", r);
		print_int("word_delta", (int)imm);
		print_u64("target", dpc + (u64)imm * 4);
		print_u64("expect_target", spc - 256);
		return 0;
	}

	if (strcmp(cmd, "class_variants") == 0) {
		/* Each of these is a known PC-relative class that must be
		 * re-encoded, not copied verbatim.  src_pc 0x2000 with imm
		 * -64 words targets 0x1F00; moving to dst_pc 0x1000 must keep
		 * that absolute target, so the imm field has to change.  The
		 * keep_mask/keep_expect pair pins the bits that must survive:
		 * CBNZ/TBNZ bit[24], TBZ b40, and the SIMD literal's opc[31:30]
		 * and V[26]. */
		const struct {
			const char *name;
			u32 base;
			u32 imm_mask;
			unsigned int imm_bits;
			u32 keep_mask;
			u32 keep_expect;
			s64 imm;
		} cases[] = {
			{ "cbz", 0x34000000u, 0x00FFFFE0u, 19, 0xFF00001Fu,
			  0x34000000u, -64 },
			{ "cbnz", 0x35000000u, 0x00FFFFE0u, 19, 0xFF00001Fu,
			  0x35000000u, -64 },
			{ "tbz", 0x36000000u, 0x0007FFE0u, 14, 0xFFF8001Fu,
			  0x36000000u, -64 },
			{ "tbnz", 0x37000000u, 0x0007FFE0u, 14, 0xFFF8001Fu,
			  0x37000000u, -64 },
			{ "ldr_simd_lit", 0x5C000000u, 0x00FFFFE0u, 19,
			  0xFF00001Fu, 0x5C000000u, -64 },
		};
		const u64 spc = 0x2000ULL, dpc = 0x1000ULL;
		size_t i;

		for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			u32 insn = cases[i].base |
				((((u32)cases[i].imm) << 5) &
				 cases[i].imm_mask);
			u32 dst = 0;
			int ret = dh_insn_relocate(&insn, &dst,
						   (void *)(uintptr_t)spc,
						   (void *)(uintptr_t)dpc, 1);
			s64 new_imm = sext_field((dst & cases[i].imm_mask) >> 5,
						 cases[i].imm_bits);
			u64 target = dpc + (u64)new_imm * 4;
			u64 expect = spc + (u64)cases[i].imm * 4;

			printf("%s_ret=%d\n", cases[i].name, ret);
			printf("%s_dst=0x%08" PRIx32 "\n", cases[i].name, dst);
			printf("%s_changed=%d\n", cases[i].name, dst != insn);
			printf("%s_class_ok=%d\n", cases[i].name,
			       (dst & cases[i].keep_mask) ==
			       cases[i].keep_expect);
			printf("%s_target=0x%016" PRIx64 "\n", cases[i].name,
			       target);
			printf("%s_expect=0x%016" PRIx64 "\n", cases[i].name,
			       expect);
		}
		return 0;
	}

#ifdef DH_INS_TEST_SEAM
	if (strcmp(cmd, "pair_public") == 0) {
		u32 out[2] = { 0, 0 };
		u32 adrp = mk_adrp(0x10000, 0);
		u32 follow = mk_add(0x20, 0, 1, 2);
		int ret = dh_insn_retarget_pair(
			adrp, follow, (void *)(uintptr_t)0x10000000ULL,
			(void *)(uintptr_t)0x20000000ULL,
			(void *)(uintptr_t)0x30000000ULL, out);

		printf("pair_public_ret=%d\n", ret);
		printf("pair_public_adrp=0x%08" PRIx32 "\n", out[0]);
		printf("pair_public_follow=0x%08" PRIx32 "\n", out[1]);
		printf("pair_public_null=%d\n",
		       dh_insn_retarget_pair(
			       adrp, follow, (void *)(uintptr_t)0x10000000ULL,
			       (void *)(uintptr_t)0x20000000ULL,
			       (void *)(uintptr_t)0x30000000ULL, NULL));
		return 0;
	}
	if (strcmp(cmd, "reloc_out") == 0) {
		u32 in = 0x14000000U; /* B +0 */
		u32 out[4] = { 0, 0, 0, 0 };
		size_t nr = 99, nr0 = 99;
		int ret = dh_insn_relocate_out(
			&in, out, 4, &nr, (void *)(uintptr_t)0x1000ULL,
			(void *)(uintptr_t)0x2000ULL, 1);
		int cap = dh_insn_relocate_out(
			&in, out, 0, &nr0, (void *)(uintptr_t)0x1000ULL,
			(void *)(uintptr_t)0x2000ULL, 1);

		printf("reloc_out_ret=%d\n", ret);
		printf("reloc_out_nr=%zu\n", nr);
		printf("reloc_out_cap=%d\n", cap);
		printf("reloc_out_nr_after_cap=%zu\n", nr0);
		return 0;
	}

	if (strcmp(cmd, "pair") == 0) {
		/* Every case uses ADRP at 0x10000000 pointing at page
		 * 0x20000000 (the old array) and moves the array to 0x30000000.
		 * xor_* must stay inside the documented imm masks, which is the
		 * direct proof that sh/size/opc/V/Rd/Rn/Rt are untouched. */
		const struct {
			const char *name;
			u32 adrp;
			u32 follow;
			u64 old_base;
			u64 new_base;
		} cases[] = {
			{ "add_sh0", mk_adrp(0x10000, 0), mk_add(0x20, 0, 1, 2),
			  0x20000000ULL, 0x30000000ULL },
			{ "add_sh0_shifted", mk_adrp(0x10000, 0),
			  mk_add(0x20, 0, 1, 2), 0x20000000ULL,
			  0x30000040ULL },
			{ "add_sh1", mk_adrp(0x10000, 0), mk_add(1, 1, 1, 2),
			  0x20000000ULL, 0x30000000ULL },
			{ "add_sh1_misaligned", mk_adrp(0x10000, 0),
			  mk_add(1, 1, 1, 2), 0x20000000ULL, 0x30000020ULL },
			{ "ldr_x", mk_adrp(0x10000, 0), mk_ldr_x(4, 2, 3),
			  0x20000000ULL, 0x30000000ULL },
			{ "ldr_x_shifted", mk_adrp(0x10000, 0), mk_ldr_x(4, 2, 3),
			  0x20000000ULL, 0x30000040ULL },
			{ "ldrb", mk_adrp(0x10000, 0), mk_ldrb(0x20, 4, 5),
			  0x20000000ULL, 0x30000000ULL },
			{ "far", mk_adrp(0x10000, 0), mk_add(0x20, 0, 1, 2),
			  0x20000000ULL, 0x10000000ULL + (1ULL << 33) },
			{ "scale", mk_adrp(0x10000, 0), mk_ldr_x(4, 2, 3),
			  0x20000000ULL, 0x30000004ULL },
			{ "nop_follower", mk_adrp(0x10000, 0), 0xD503201Fu,
			  0x20000000ULL, 0x30000000ULL },
			{ "sub_follower", mk_adrp(0x10000, 0),
			  mk_sub(0x20, 0, 1, 2), 0x20000000ULL,
			  0x30000000ULL },
			/* ADDS shares ADD (immediate)'s imm12 field; only the S
			 * bit[29] differs.  It must be retargeted like ADD. */
			{ "adds_follower", mk_adrp(0x10000, 0),
			  mk_adds(0x20, 0, 1, 2), 0x20000000ULL,
			  0x30000000ULL },
			/* SUBS is still a subtract: no unsigned imm12 can reach
			 * an arbitrary lower in-page offset, so it is refused. */
			{ "subs_follower", mk_adrp(0x10000, 0),
			  mk_subs(0x20, 0, 1, 2), 0x20000000ULL,
			  0x30000000ULL },
			/* Backward move: the new page is below both the old array
			 * and the ADRP's own page, so the ADRP delta is negative.
			 * It is in range and must encode, not be refused. */
			{ "backward", mk_adrp(0x10000, 0),
			  mk_add(0x20, 0, 1, 2), 0x20000000ULL,
			  0x0F000000ULL },
			{ "bad_adrp", 0xD503201Fu, mk_add(0x20, 0, 1, 2),
			  0x20000000ULL, 0x30000000ULL },
		};
		const u64 pc = 0x10000000ULL;
		size_t i;

		for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			u32 out_a = 0, out_f = 0;
			int ret = dh_ins_test_pair_retarget(cases[i].adrp,
							   cases[i].follow, pc,
							   cases[i].old_base,
							   cases[i].new_base,
							   &out_a, &out_f);

			printf("case_%s_ret=%d\n", cases[i].name, ret);
			if (ret != DH_OK)
				continue;
			printf("case_%s_dst_adrp=0x%08" PRIx32 "\n",
			       cases[i].name, out_a);
			printf("case_%s_dst_follow=0x%08" PRIx32 "\n",
			       cases[i].name, out_f);
			printf("case_%s_xor_adrp=0x%08" PRIx32 "\n",
			       cases[i].name, out_a ^ cases[i].adrp);
			printf("case_%s_xor_follow=0x%08" PRIx32 "\n",
			       cases[i].name, out_f ^ cases[i].follow);
			printf("case_%s_imm12=%d\n", cases[i].name,
			       (int)((out_f >> 10) & 0xFFF));
			printf("case_%s_sh=%d\n", cases[i].name,
			       (int)((out_f >> 22) & 1));
			printf("case_%s_opc=%d\n", cases[i].name,
			       (int)((out_f >> 22) & 3));
			printf("case_%s_size=%d\n", cases[i].name,
			       (int)((out_f >> 30) & 3));
			printf("case_%s_pages=%d\n", cases[i].name,
			       (int)decode_adrp_pages(out_a));
		}
		print_int("r_null_out",
			  dh_ins_test_pair_retarget(cases[0].adrp,
						    cases[0].follow, pc,
						    0x20000000ULL,
						    0x30000000ULL,
						    NULL, NULL));
		print_int("inval", DH_E_INVAL);
		return 0;
	}
#endif

	if (strcmp(cmd, "scan_bounds") == 0) {
		/* Boundary of the 99995a6 invariant: a reference exactly at
		 * old_base is inside, one exactly at old_base+old_len is not. */
		u64 text_page = (u64)(uintptr_t)g_text & ~0xFFFULL;
		u64 old = text_page + (16ULL << 12);
		s64 dp = (s64)((old - text_page) >> 12);
		void *sites[8];
		int n;

		emit("scan_bounds");
		g_text[0] = mk_adrp(dp, 0);
		g_text[1] = mk_add(0, 0, 0, 0);       /* old + 0: inside */
		g_text[2] = mk_adrp(dp, 2);
		g_text[3] = mk_add(64, 0, 2, 2);      /* old + 64: outside */
		g_probe_stext = (void *)(uintptr_t)g_text;
		g_probe_etext = (void *)(uintptr_t)(g_text + 16);
		n = dh_scan_refs(&g_ctx, (void *)(uintptr_t)old, 64, NULL,
				 sites, 8);
		print_int("n", n);
		print_u64("site0", (u64)(uintptr_t)sites[0]);
		print_u64("expect0", (u64)(uintptr_t)&g_text[0]);
		return 0;
	}

	if (strcmp(cmd, "scan_unencodable") == 0) {
		/* In-range reference that cannot be re-encoded for new_base
		 * (0x1004 shifts the target by 4 bytes; an LDR offset must be a
		 * multiple of 8).  The scan must fail closed, not drop it. */
		u64 text_page = (u64)(uintptr_t)g_text & ~0xFFFULL;
		u64 old = text_page + (16ULL << 12);
		s64 dp = (s64)((old - text_page) >> 12);
		void *sites[8];
		int n;

		emit("scan_unencodable");
		g_text[0] = mk_adrp(dp, 0);
		g_text[1] = mk_ldr_x(4, 2, 3);        /* old + 0x20 */
		g_probe_stext = (void *)(uintptr_t)g_text;
		g_probe_etext = (void *)(uintptr_t)(g_text + 16);
		n = dh_scan_refs(&g_ctx, (void *)(uintptr_t)old, 64,
				 (void *)(uintptr_t)(old + 0x1004), sites, 8);
		print_int("n", n);
		print_int("opnotsupp", DH_E_OPNOTSUPP);
		return 0;
	}

	fprintf(stderr, "unknown subcommand: %s\n", cmd);
	return 2;
}
