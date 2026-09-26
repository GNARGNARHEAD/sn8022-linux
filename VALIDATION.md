# Portable package validation — 26 September 2026

This is an experimental source release candidate for one tested laptop and
Ubuntu 7.0.0-34-generic/x86_64.

## Completed

- All five driver source files match the installed, user-accepted 0.2.1
  checkpoint byte for byte. Amplifier sources remain unchanged from 0.2.0.
- A fresh extraction of the source archive passed real DKMS 3.2.2 add/build/sign
  in an isolated writable DKMS tree using the distribution kernel headers.
  The output reports srcversion `3347A8152C7B5E35BF1B9E4`.
- The verifier checked that real module's identity and CMS signature against
  exported, enrolled public MOK certificates. No private key is distributed or
  read by the verifier. A separate read-only check accepted the installed module.
- A synthetic initramfs containing the built module and the separately supplied
  local profile was created with cpio/gzip, extracted with real `unmkinitramfs`,
  and accepted by the payload verifier. These binary fixtures are not distributed.
- The private, never-installed DKMS registration was removed successfully.
- Twenty offline tests pass: source tampering/path checks, DKMS framework policy,
  exact initramfs locations/content, registration mismatch, command ordering,
  failure before module replacement, boot-image failure, retained rollback data,
  and the removal transaction method without hardware/header checks.
- The archive contains only explicitly listed source, licenses, documentation and
  tests. Its source manifest and archive SHA-256 accompany it. Archive generation
  is deterministic; build products and personal development data are excluded.

## Limits and remaining checks

The lifecycle tests simulate DKMS and boot-image commands while exercising the
actual Python transaction methods. Transaction construction, package locks, backup
creation and finalization are not covered by that simulated suite. The CLI
branch that permits removal without headers/hardware was source-reviewed. These
tests **do not establish successful privileged
installation, upgrade, removal or rollback on a clean machine**. The real DKMS
exercise above covered add/build/sign and removal of a never-installed private
registration only. Validate the complete lifecycle on a disposable matching
Ubuntu installation before treating the installer as production-ready.

That additional trial is deferred to future volunteer installation testing.
The completed driver checks and the untested installer lifecycle remain distinct.

The new wrapper has not been installed over the working development driver.
That local package has different signing and recovery assumptions, and is
intentionally refused by the portable installer. No audio service or running
module was changed during packaging validation.

Attended 0.2.1 speaker/idle playback and headphone insertion/removal passed,
including clean left/right speaker return, volume/mute and retained internal-mic selection;
see `TESTING.md`. The broader checklist, including new sleep cycles on 0.2.1,
remains open. Existing 0.2.0 sleep and 0.2.1 microphone observations do not
replace those checks. External headset-microphone behavior and another physical
laptop remain untested.

Firmware redistribution permission remains unresolved. The source archive
includes a local extractor and identity checks, not the OEM profile itself.
