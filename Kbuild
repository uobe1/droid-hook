# SPDX-License-Identifier: GPL-2.0-only
#
# Kernel module build for Droid-Hook.
#
#   obj-m := droid_hook.o
#
# The module is the library plus packaging: src/ (kernel-agnostic library) and
# dh_module.c (module_init/exit, exports, self-test).  runtime/ is the optional
# core RW ABI 2 adapter: it needs the droid-lkm-suite core headers, so it is
# compiled in only when DNSK_CORE_INC points at a directory containing
# dnsk_core.h.  Without it the module still builds as library + packaging.
#
#   make -C <kernel tree> M=$PWD modules
#   make -C <kernel tree> M=$PWD DNSK_CORE_INC=<core>/include modules

obj-m := droid_hook.o

droid_hook-y := \
	src/dh_rw.o \
	src/dh_txn.o \
	src/dh_insn.o \
	src/dh_hook.o \
	src/dh_shadow.o \
	src/dh_preflight.o \
	dh_module.o

# Fail closed on a wrong path: a non-empty DNSK_CORE_INC that does not contain
# dnsk_core.h is a configuration error, not a silent downgrade to a build
# without the runtime adapter.
RUNTIME_CORE_HDR := $(wildcard $(DNSK_CORE_INC)/dnsk_core.h)
ifneq ($(strip $(RUNTIME_CORE_HDR)),)
droid_hook-y += runtime/dh_core_backend.o runtime/dh_ro_routes.o
ccflags-y += -I$(DNSK_CORE_INC) -I$(src)/runtime
# dh_module.c then also runs the runtime split-path self-test against a mock
# core API.  The macro is what keeps that code (and the core header include)
# out of the plain build.
ccflags-y += -DDH_WITH_RUNTIME=1
$(info droid-hook: DNSK_CORE_INC set -> runtime/dh_core_backend.o included)
else ifneq ($(strip $(DNSK_CORE_INC)),)
$(error droid-hook: DNSK_CORE_INC="$(DNSK_CORE_INC)" contains no dnsk_core.h)
else
$(info droid-hook: DNSK_CORE_INC unset -> runtime/dh_core_backend.o skipped)
endif

ccflags-y += -I$(src)/include
ccflags-y += -Wno-declaration-after-statement
