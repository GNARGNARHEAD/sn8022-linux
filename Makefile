# SPDX-License-Identifier: GPL-2.0-or-later
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

.PHONY: all clean
all:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" modules
clean:
	$(MAKE) -C "$(KDIR)" M="$(CURDIR)" clean
