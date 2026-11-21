# SPDX-License-Identifier: GPL-2.0-only
#
# Kernel module build for Droid-Hook.
#
#   obj-m := droid_hook.o
#
# The module is the library plus packaging: src/ (kernel-agnostic library) and
# dh_module.c (module_init/exit, exports, self-test).  runtime/ is NOT part of
# this module: the core RW ABI adapter needs the droid-lkm-suite core headers,
# so it is built separately with DNSK_CORE_INC pointing at them (see Makefile).
#
#   make -C <kernel tree> M=$PWD modules
#
# This Kbuild only packages the library TUs and dh_module.c; it does not build
# runtime/ and does not read DNSK_CORE_INC.

obj-m := droid_hook.o

droid_hook-y := \
	src/dh_rw.o \
	src/dh_txn.o \
	src/dh_insn.o \
	src/dh_hook.o \
	src/dh_shadow.o \
	src/dh_preflight.o \
	dh_module.o

ccflags-y += -I$(src)/include
ccflags-y += -Wno-declaration-after-statement
