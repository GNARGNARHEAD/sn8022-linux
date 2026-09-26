#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Relevant DKMS 3.2.2 safe_source semantics: every file runs in a subshell;
# unset variables produce no assignment, preserving the previous outer value.
set -e
dkms_tree=/var/lib/dkms
source_tree=/usr/src
install_tree=/lib/modules
tmp_location=${TMPDIR:-/tmp}
verbose=
symlink_modules=
compress_gzip_opts=
compress_xz_opts='--check=crc32 --lzma2=dict=1MiB'
compress_zstd_opts='-q --rm -T0'
modprobe_on_install=${modprobe_on_install-}
post_transaction=${post_transaction-}
variables=(source_tree dkms_tree install_tree tmp_location verbose symlink_modules
    autoinstall_all_kernels modprobe_on_install parallel_jobs compress_gzip_opts
    compress_xz_opts compress_zstd_opts post_transaction)
for name in ${!DKMS_DIRECTIVE@}; do
    exit 1
done
temporary=$(mktemp)
trap 'rm -f -- "$temporary"' EXIT
for file in /etc/dkms/framework.conf /etc/dkms/framework.conf.d/*.conf; do
    [ -e "$file" ] || continue
    (
        . "$file" >/dev/null
        for name in "${variables[@]}"; do
            if declare -p "$name" &>/dev/null; then declare -p "$name"; fi
        done
        # DKMS permits arbitrary directives in framework configuration. This
        # package refuses them rather than running substituted module hooks.
        for name in ${!DKMS_DIRECTIVE@}; do
            printf 'unsupported_directive=1\n'
        done
    ) > "$temporary"
    . "$temporary"
done
[ -z "${unsupported_directive-}" ]
[ "$dkms_tree" = /var/lib/dkms ]
[ "$source_tree" = /usr/src ]
[ "$install_tree" = /lib/modules ]
[ -z "$symlink_modules" ]
[ -z "$modprobe_on_install" ]
[ -z "$post_transaction" ]
