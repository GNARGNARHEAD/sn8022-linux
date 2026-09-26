# Upstream source provenance

Bundled Conexant baseline and private HDA headers originate from Ubuntu
`linux 7.0.0-31.31`; the required source/header files were compared with
`7.0.0-34.34` and found byte-identical before the current build.

Repository: https://git.launchpad.net/~ubuntu-kernel/ubuntu/+source/linux/+git/resolute

Tag: `Ubuntu-7.0.0-31.31`

Commit: `68d75f0820869e33326712a9547550961795eaaf`

The original Conexant authors and helper authors remain credited in their
source files. Local changes add the exact-board SN8022 amplifier lifecycle,
firmware validation, conventional speaker controls, fast runtime resume and
microphone selection. Build headers/helpers are unchanged.

The original source archives were verified locally against their package
metadata. Shared upstream archive identities:

- `linux_7.0.0-31.31.diff.gz`: `7d1ee5c5a554dc037f9d1ce643b644edff63c01718a4fa1d5c735e064b33cd6a`
- `linux_7.0.0.orig.tar.gz`: `c6343795c8c7e4feff55f5b309365c8e0eaf49cd6abcb412e32315313f149536`

Private build-input hashes are in `include/MANIFEST.sha256`. The full release
manifest covers all distributed source files. No local paths, host captures or
signing identities are needed to identify these upstream sources.
