# SPDX-License-Identifier: GPL-3.0-or-later
#
# Droid-Hook build entry points.
#
#   make                  same as `make check` (host gate)
#   make check            compile every TU (host flags) + run every host suite
#   make test             host suites only
#   make module           build droid_hook.ko against KDIR (a prepared kernel tree)
#   make module-clean     remove the kernel module build output
#   make kcheck           kernel-mode syntax check of the library TUs, using the
#                         AOSP headers under AOSP= (no kernel tree needed)
#   make clean
#
# `make kcheck` is what can be validated on a machine without a kernel build
# tree: it proves the library layer stays kernel-clean.  dh_module.c needs
# generated headers (<asm/cpucap-defs.h> and friends) so only a real kernel
# build (CI / DDK) can validate it.

CC       ?= cc
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror
INCS     := -Iinclude -Iruntime
BUILDD   := build/host

SRCS     := $(sort $(wildcard src/*.c) $(wildcard runtime/*.c))
OBJS     := $(patsubst %.c,$(BUILDD)/%.o,$(SRCS))
TESTS    := $(sort $(wildcard tests/host/test_*.py))

KDIR     ?= /lib/modules/$(shell uname -r)/build
AOSP     ?= /usr/local/share/aosp-6.12.30
LIB_SRCS := $(sort $(wildcard src/*.c))

# Optional path to droid-lkm-suite's core headers, e.g.
#   DNSK_CORE_INC=/path/to/droid-lkm-suite/core/include
# When set, the runtime adapter (runtime/dh_core_backend.c) is compiled into
# droid_hook.ko; when empty it is skipped and the module is library +
# packaging only.  Kbuild rejects a non-empty path without dnsk_core.h.
DNSK_CORE_INC ?=

.PHONY: all check compile test kcheck module module-clean runtime-check clean

all: check

check: compile test
	@echo "droid-hook: host gate clean ($(words $(SRCS)) TUs, $(words $(TESTS)) suites)"

compile: $(OBJS)

$(BUILDD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCS) -c $< -o $@

test:
	@set -e; for t in $(TESTS); do \
		echo "== $$t"; \
		(cd tests/host && python3 -m unittest -v "$$(basename $$t .py)") || exit 1; \
	done

# Kernel-mode syntax check of the library TUs.  Catches a TU that quietly
# depends on a libc header the kernel does not have, before CI spends a build.
kcheck:
	@set -e; for f in $(LIB_SRCS); do \
		printf '  %-24s' "$$f"; \
		$(CC) -std=gnu11 -D__KERNEL__ -fsyntax-only $(INCS) \
			-I$(AOSP)/include -I$(AOSP)/arch/arm64/include "$$f" \
			&& echo ok || { echo FAIL; exit 1; }; \
	done

module:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" DNSK_CORE_INC="$(DNSK_CORE_INC)" modules

module-clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean

# Kernel-mode syntax check of the runtime adapter against the real core
# header: the strongest check possible without a prepared kernel tree.  It
# proves runtime/dh_core_backend.c includes <dnsk_core.h> cleanly under
# -D__KERNEL__ and does not leak libc headers.
runtime-check:
	@test -n "$(DNSK_CORE_INC)" || { \
		echo "runtime-check: DNSK_CORE_INC is required, e.g. DNSK_CORE_INC=/path/to/droid-lkm-suite/core/include" >&2; \
		exit 1; }
	@test -f "$(DNSK_CORE_INC)/dnsk_core.h" || { \
		echo "runtime-check: $(DNSK_CORE_INC)/dnsk_core.h not found" >&2; exit 1; }
	$(CC) -std=gnu11 -D__KERNEL__ -fsyntax-only -Iinclude -Iruntime \
		-I$(DNSK_CORE_INC) -I$(AOSP)/include -I$(AOSP)/arch/arm64/include \
		runtime/dh_core_backend.c
	@echo "runtime-check: runtime/dh_core_backend.c ok against $(DNSK_CORE_INC)"

clean:
	rm -rf $(BUILDD)
