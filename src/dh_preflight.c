/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Droid-Hook preflight and protection probe.
 *
 * dh.h declared these two entry points before they existed; this file makes
 * them real rather than leaving the header lying.  Both are cheap and safe to
 * call before any patch is attempted.
 *
 *   dh_preflight()        validate the ctx/backend pair and that no transaction
 *                         is half-open, so a caller learns about a broken setup
 *                         before the first write instead of halfway through.
 *   dh_probe_protection() learn whether the read-only write route works on a
 *                         target by performing a same-byte write through it and
 *                         reading the word back.
 *
 * Why a same-byte write is safe and honest: the bytes written are exactly the
 * bytes read from the same address, so a successful route leaves the target's
 * content and instruction stream identical; the only observable effect is that
 * the route demonstrably accepted the write.  A corruption bug in the route
 * shows up as a read-back mismatch (DH_E_VERIFY) instead of silently succeeding,
 * and a refusing route returns its real errno instead of a forged DH_OK.  The
 * probe therefore never fakes a result: read failure, write failure and
 * verification failure are each propagated unchanged.
 *
 * Only DH_ROUTE_FIXMAP is probed because the core RW ABI 2 adapter implements
 * exactly that route today (F_RO -> patch_map); ALIAS/PTE/DMA are contract
 * placeholders in the adapter.  When the adapter distinguishes more routes this
 * probe should iterate them in preference order; until then, probing the one
 * real route is the honest check.
 */

#include "dh.h"

int dh_probe_protection(struct dh_ctx *ctx, void *addr, enum dh_route *route)
{
	u8 before[sizeof(u32)];
	u8 after[sizeof(u32)];
	size_t len = sizeof(u32);
	int ret;

	if (ctx == NULL || ctx->be == NULL || addr == NULL || route == NULL)
		return DH_E_INVAL;
	if (ctx->initialized == 0)
		return DH_E_INVAL;

	/*
	 * dh_ctx_init() already guarantees these are present for any usable ctx;
	 * keep the guard so a backend swapped out under a live ctx can never turn
	 * a missing route into a silent success.
	 */
	if (ctx->be->ro_write == NULL || ctx->be->rw == NULL)
		return DH_E_OPNOTSUPP;

	/* One arm64 instruction word: the granularity text patches work on, and
	 * small enough that a probe near a page boundary cannot over-read. */
	ret = dh_read(ctx, addr, before, len);
	if (ret != DH_OK)
		return ret;                    /* real read error, propagated */

	ret = ctx->be->ro_write(addr, before, len, DH_ROUTE_FIXMAP);
	if (ret != DH_OK) {
		*route = DH_ROUTE_FIXMAP;      /* which route was attempted */
		return ret;                    /* real write error, propagated */
	}

	ret = dh_read(ctx, addr, after, len);
	if (ret != DH_OK)
		return ret;

	if (__builtin_memcmp(before, after, len) != 0)
		return DH_E_VERIFY;

	*route = DH_ROUTE_FIXMAP;
	return DH_OK;
}

int dh_preflight(struct dh_ctx *ctx)
{
	if (ctx == NULL || ctx->be == NULL)
		return DH_E_INVAL;
	if (ctx->initialized == 0 || ctx->be->abi != DH_ABI)
		return DH_E_INVAL;
	if (ctx->be->rw == NULL || ctx->be->probe == NULL)
		return DH_E_INVAL;
	if (ctx->txn.active != 0)
		return DH_E_BUSY;
	return DH_OK;
}
