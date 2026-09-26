# SPDX-License-Identifier: GPL-2.0-or-later
# External build of the ordinary Conexant module. No installation target.
obj-m := snd-hda-codec-conexant.o
SN8022_OBJECTS ?= conexant.o sn8022.o sn8022_core.o
snd-hda-codec-conexant-y := $(SN8022_OBJECTS)
ccflags-y += -I$(src)/include
