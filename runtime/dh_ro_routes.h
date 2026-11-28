/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DH_RO_ROUTES_H
#define _DH_RO_ROUTES_H

#include "dh.h"

/*
 * Write to a read-only target over one concrete route.
 *
 * ALIAS and PTE are real implementations in the kernel build.  FIXMAP is not
 * handled here: the core ABI 2 adapter still delegates it to dnsk_core's
 * patch backend.  DMA is fail-closed (DH_E_OPNOTSUPP) because a normal LKM
 * has no EL2/Stage-2 control and no guaranteed DMA engine; returning an error
 * is honest, while pretending DMA succeeded would not be.
 *
 * The caller is responsible for the transaction/quiesce protocol and for
 * read-back verification.  Returns DH_OK or a negative DH_E_* / errno.
 */
int dh_ro_route_write(void *addr, const void *src, size_t len, enum dh_route route);

/*
 * dh_ro_set_ro() / dh_ro_set_rw() - change page permissions for the PTE route
 *
 * Thin wrappers over set_memory_ro()/set_memory_rw().  Those two functions
 * are not exported to modules on every GKI build, so the wrappers resolve
 * them through kallsyms (kprobe) on first use and return
 * DH_E_OPNOTSUPP when they are unavailable.  Callers must not import
 * set_memory_ro/set_memory_rw directly: doing so makes the whole module
 * refuse to load with "Unknown symbol".
 */
int dh_ro_set_ro(unsigned long addr, int npages);
int dh_ro_set_rw(unsigned long addr, int npages);

#endif /* _DH_RO_ROUTES_H */
