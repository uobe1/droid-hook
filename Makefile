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

.PHONY: all check compile test kcheck module module-clean clean

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
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules

module-clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean

clean:
	rm -rf $(BUILDD)
