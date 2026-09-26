#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu
kernel=$1
case "$kernel" in ''|*[!a-zA-Z0-9._+-]*) echo 'Invalid kernel name' >&2; exit 1;; esac
depmod -a "$kernel"
if [ -f "/boot/initrd.img-$kernel" ]; then
    update-initramfs -u -k "$kernel"
fi
