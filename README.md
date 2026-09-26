# SN8022 audio support — experimental 0.2.1

An extension of Linux's ordinary Conexant HDA driver for the **CHUWI CoreBook
Air Plus / ARN25C**, codec **14f1:20d1**, codec subsystem **2014:7054**.
It provides speaker playback and usable built-in microphone selection through
standard ALSA/PipeWire controls. There is no audio daemon or background probe.

This first package supports **Ubuntu kernel 7.0.0-34-generic, x86_64**, with the
distribution build inputs listed in `release.json`. It bundles private HDA
headers and therefore rejects unchecked kernels. This is a compatibility
boundary, not a request to downgrade a newer kernel. New kernel support needs
source review and testing before extending the allowlist.

Only one physical laptop has been tested. See `TESTING.md` for completed checks
and remaining regression work. Automatic headset-microphone detection is not
available: choose **Mic** manually for an external microphone and **Internal
Mic** for the built-in microphone. Headphone playback detection is separate.

## What is included

- Driver and private Linux build headers, preserving upstream notices.
- Standard DKMS configuration, a small checked installation/removal wrapper,
  firmware extractor, documentation and tests.
- No prebuilt modules, signing keys/certificates, OEM firmware, recordings,
  machine logs, unique hardware identifiers or network downloader.

The kernel implementation is identical to the tested 0.2.1 source checkpoint.
The portable installer is new; validation results distinguish isolated tests
from physical-machine installation. This archive is a release candidate, not
a published stable release.

The complete portable install/upgrade/remove lifecycle has not yet been tested
on a clean machine. This is a documented gap for experimental testers; see
`VALIDATION.md`. The existing development installation passed the attended
speaker, headphone, idle, volume/mute and built-in microphone checks.

## Prerequisites

Use the supported hardware and kernel. Install your distribution's DKMS,
matching kernel headers, build tools, Python 3, OpenSSL, mokutil, zstd and
working `depmod`, `modinfo`, `update-initramfs`, and `unmkinitramfs` commands.
The tested DKMS version is 3.2.2. The tested Ubuntu `update-initramfs` is provided
by dracut; arbitrary initramfs generators/distributions are not claimed tested.
Keep enough free space for a module build and several copies of the boot image.

The wrapper refuses an existing local/other Conexant replacement and refuses
to overwrite a different source tree with the same package version. Do not
apply this package on top of the development installation without a migration.

## Firmware supplied separately

The amplifier requires the exact 2,982-byte profile documented in `FIRMWARE.md`.
No permission to redistribute that OEM profile has been established, and it is
not included here. If you already have the reviewed OEM driver file:

```sh
python3 extract-firmware.py /path/to/cisstrtU-ext-OEM-common.inf --output /path/to/sn8022-20147054.bin
sudo python3 manage.py firmware --profile /path/to/sn8022-20147054.bin
```

Extraction checks the source identity, hardware mapping, size and complete
profile hash. Import refuses to overwrite a different installed profile. It
does not install a driver or play audio. Obtaining the OEM file and permission
to use/distribute it are separate from the source-code license.

## Install for the next normal boot

From the extracted source directory, run:

```sh
sudo python3 manage.py install
```

DKMS uses the computer owner's normal signing configuration. This package does
not install a shared key, change DKMS's global signing policy, disable Secure
Boot or bypass signature checks. If Secure Boot is enabled and the key is not
enrolled, installation stops before replacing the module. Follow `SECURE_BOOT.md`
to enroll **your DKMS public certificate**, reboot, then retry.

The wrapper checks the selected module and extracted initramfs contents after
installation. It does not reload the driver, restart audio, change the selected
kernel or reboot. It refuses DKMS configurations enabling `modprobe_on_install`
or `post_transaction`; those administrator hooks could alter the running system.

Wait for **“Verified for the next normal reboot”**, save your work, and reboot
normally. If it stops with an error, keep the machine running and inspect the
printed transaction directory before rebooting. It contains logs and backups
of the previous module and initramfs. See `RECOVERY.md`.

After reboot, check:

```sh
cat /sys/module/snd_hda_codec_conexant/srcversion
```

For this kernel/version it should report `3347A8152C7B5E35BF1B9E4`. Then follow
the short listening checklist in `TESTING.md`. In sound settings choose the
built-in speakers and Internal Microphone as appropriate.

## Upgrade, remove and recover

A later portable release can be installed with its own `manage.py install`.
The installer retains previous DKMS builds and sources for rollback; use the
sequence in `RECOVERY.md` if the new version fails. Do not use DKMS `--force`:
it can bypass failed module installation checks.

To remove this version for the next normal boot:

```sh
sudo python3 manage.py remove
```

The wrapper verifies the restored module and boot image. It leaves firmware,
signing material, sources and transaction backups in place. It refuses removal
if this version has additional kernel state outside the checked target. Other
registered versions must be dealt with explicitly; otherwise DKMS may build the
highest remaining registered version during a later kernel update.

## Build and tests without installation

```sh
python3 check.py build "$(uname -r)"
make
python3 -m unittest discover -s tests -v
```

Build in a separate copy of the package if you want to retain an untouched
source tree. The distribution tarball is created from an explicit source-file
list and excludes build output. `MANIFEST.sha256` detects changed packaged files;
it is an integrity list, not a publisher signature or proof of authenticity.

License notices and provenance are in `COPYING.md`, `LICENSES/`, and
`UPSTREAM.md`. Firmware is outside the source-code license.
