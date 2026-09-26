#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract the exact local SU4 tuning profile; no network, installation or I/O to hardware."""
import argparse
import hashlib
import json
from pathlib import Path
import re

INF_SHA256 = '1ae4cb9ac55a543be5e1eb1d516d1cecf5d39ee602d68341bba0803a64a34639'
PROFILE_SHA256 = '49ca4006dca8fa849f310b2b089efaff9ae9a24f24612ebf3e4a9d78723df214'
PROFILE_SIZE = 2982
TARGET_MAPPING = r'%Codec.DeviceDesc% = HdAud2021.SU4, HDAUDIO\FUNC_01&VEN_14F1&DEV_20D1&SUBSYS_20147054'


def extract_profile(source):
    raw = Path(source).read_bytes()
    if hashlib.sha256(raw).hexdigest() != INF_SHA256:
        raise ValueError('source is not the reviewed OEM extension INF')
    section = None
    profile = None
    mapping = 0
    for line in raw.decode('utf-8-sig').splitlines():
        line = line.split(';', 1)[0].strip()
        mapping += line == TARGET_MAPPING
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1]
        elif section == 'OEM.SU4.AddReg':
            fields = [v.strip() for v in line.split(',')]
            if fields[:4] == ['HKR', r'Settings\OEM', '8022Register', '1']:
                if profile is not None or not all(re.fullmatch('[0-9a-fA-F]{2}', v) for v in fields[4:]):
                    raise ValueError('invalid or repeated profile record')
                profile = bytes(int(v, 16) for v in fields[4:])
    if mapping != 1 or profile is None or len(profile) != PROFILE_SIZE:
        raise ValueError('missing or ambiguous target/profile')
    if hashlib.sha256(profile).hexdigest() != PROFILE_SHA256:
        raise ValueError('profile differs from the reviewed 2014:7054 tuning')
    return profile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inf', type=Path)
    parser.add_argument('--output', type=Path, required=True,
                        help='local staging file, usually conexant/sn8022-20147054.bin')
    args = parser.parse_args()
    try:
        data = extract_profile(args.inf)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        if args.output.exists():
            if args.output.read_bytes() != data:
                raise ValueError('refusing to overwrite a different existing output')
        else:
            with args.output.open('xb') as f:
                f.write(data)
        print(json.dumps({'path': str(args.output.resolve()), 'size': len(data),
                          'sha256': PROFILE_SHA256, 'source_sha256': INF_SHA256,
                          'installed': False}, indent=2))
    except (OSError, UnicodeError, ValueError) as err:
        parser.exit(1, f'{err}\n')


if __name__ == '__main__':
    main()
