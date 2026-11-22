// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only write routes for the runtime adapter.
 *
 * ALIAS: map the target page through a fresh writable vmap alias, copy, drop
 *        the alias.  The original PTE is never modified.
 * PTE:   temporarily make the target pages writable with set_memory_rw(),
 *        copy, then restore with set_memory_ro().
 * FIXMAP: delegated to dnsk_core by dh_core_backend.c (not here).
 * DMA:   fail-closed.  A normal LKM cannot drive EL2/Stage-2, and there is no
 *        guaranteed DMA engine; DH_E_OPNOTSUPP is the honest answer.
 *
 * Both implemented routes finish with an icache flush so a caller that patches
 * executable text sees the new instructions.  Data targets pay a harmless
 * cache maintenance cost.
 */

#include "dh_ro_routes.h"

#ifdef __KERNEL__

#include <linux/mm.h>
#include <linux/set_memory.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <asm/cacheflush.h>
#include <asm/page.h>
#include <asm/pgtable.h>

static int dh_ro_alias_write(void *addr, const void *src, size_t len)
{
	u8 *a = addr;
	const u8 *s = src;

	while (len > 0) {
		struct page *page;
		void *alias;
		size_t off = (unsigned long)a & ~PAGE_MASK;
		size_t n = len < (PAGE_SIZE - off) ? len : (PAGE_SIZE - off);

		page = vmalloc_to_page(a);
		if (page == NULL)
			page = virt_to_page(a);
		alias = vmap(&page, 1, VM_MAP, PAGE_KERNEL);
		if (alias == NULL)
			return DH_E_NOSPC;
		memcpy((u8 *)alias + off, s, n);
		vunmap(alias);

		a += n;
		s += n;
		len -= n;
	}
	return DH_OK;
}

static int dh_ro_pte_write(void *addr, const void *src, size_t len)
{
	unsigned long start = (unsigned long)addr & PAGE_MASK;
	unsigned long end = ((unsigned long)addr + len + PAGE_SIZE - 1) &
			    PAGE_MASK;
	int npages = (int)((end - start) >> PAGE_SHIFT);
	int ret;

	ret = set_memory_rw(start, npages);
	if (ret != 0)
		return ret;
	memcpy(addr, src, len);
	ret = set_memory_ro(start, npages);
	return ret != 0 ? ret : DH_OK;
}

int dh_ro_route_write(void *addr, const void *src, size_t len,
		      enum dh_route route)
{
	int ret;

	if (addr == NULL || src == NULL || len == 0)
		return DH_E_INVAL;
	switch (route) {
	case DH_ROUTE_ALIAS:
		ret = dh_ro_alias_write(addr, src, len);
		break;
	case DH_ROUTE_PTE:
		ret = dh_ro_pte_write(addr, src, len);
		break;
	default:
		/* DMA has no honest LKM implementation; FIXMAP stays in core. */
		return DH_E_OPNOTSUPP;
	}
	if (ret != DH_OK)
		return ret;
	flush_icache_range((unsigned long)addr, (unsigned long)addr + len);
	return DH_OK;
}

#else /* !__KERNEL__ */

int dh_ro_route_write(void *addr, const void *src, size_t len,
		      enum dh_route route)
{
	(void)addr;
	(void)src;
	(void)len;
	(void)route;
	return DH_E_OPNOTSUPP;
}

#endif /* __KERNEL__ */
