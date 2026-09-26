# Separately supplied SN8022 profile

Required path: `/lib/firmware/conexant/sn8022-20147054.bin`.

- Size: 2,982 bytes.
- SHA-256: `49ca4006dca8fa849f310b2b089efaff9ae9a24f24612ebf3e4a9d78723df214`.
- Reviewed OEM INF SHA-256:
  `1ae4cb9ac55a543be5e1eb1d516d1cecf5d39ee602d68341bba0803a64a34639`.
- Mapping: `HDAUDIO\FUNC_01&VEN_14F1&DEV_20D1&SUBSYS_20147054`, SU4 profile.

`extract-firmware.py` reads only a local file provided by the owner. It performs
no network access, installation or hardware commands. Do not substitute another
model's tuning profile. The driver rejects a missing or different profile and
does not enable the speakers from an unverified configuration.

OEM firmware and Windows driver extracts are excluded from the release archive.
The GPL notices on the source code do not grant rights to that OEM material.
Redistribution permission remains unresolved. A source-only archive with a
local extractor does not itself establish permission to redistribute the profile.
