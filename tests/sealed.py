#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Generate a disposable sealed APFS image and check catalog and file reads."""
import argparse
import hashlib
from pathlib import Path
import plistlib
import re
import subprocess
import tempfile

SEALER = '/System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_prepare_cryptex'


def payload(index):
    return hashlib.sha256(str(index).encode()).digest() * 200


def make_image(image):
    if image.exists() or image.suffix != '.dmg':
        raise ValueError('Choose a new .dmg path')
    subprocess.run(['hdiutil', 'create', '-size', '64m', '-layout', 'GPTSPUD',
                    '-fs', 'APFS', '-volname', 'SealedReadFixture', image], check=True)
    devices = plistlib.loads(subprocess.check_output(
        ['hdiutil', 'attach', '-nobrowse', '-plist', image]))['system-entities']
    whole = next(d['dev-entry'] for d in devices if d.get('content-hint') == 'GUID_partition_scheme')
    try:
        volume = next(d for d in devices if d.get('volume-kind') == 'apfs')
        directory = Path(volume['mount-point']) / 'Known'
        directory.mkdir()
        # Enough catalog and physical extent records to require internal nodes.
        for index in range(256):
            (directory / f'f{index:03}').write_bytes(payload(index))
        subprocess.run(['diskutil', 'unmount', volume['dev-entry']], check=True)
        seal = subprocess.run([SEALER, volume['dev-entry']], capture_output=True)
        if seal.returncode:
            raise RuntimeError(seal.stderr.decode(errors='replace'))
        info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', volume['dev-entry']]))
        if info.get('Sealed') not in (True, 'Yes'):
            raise ValueError('macOS did not report a sealed fixture')
    finally:
        subprocess.run(['hdiutil', 'detach', whole], check=True)


def verify(reader, image, work):
    before = hashlib.sha256(image.read_bytes()).digest()
    cmd = [reader, '--image', image, '512']
    inventory = subprocess.check_output(cmd, text=True)
    volume = re.search(r'^partition=(\d+) volume=(\d+) .*locked=0 name=SealedReadFixture$',
                       inventory, re.MULTILINE)
    if not volume:
        raise ValueError(inventory)
    selected = cmd + list(volume.groups())
    names = subprocess.check_output([*selected, '/Known'], text=True).splitlines()[2:]
    if sorted(names) != [f'f{i:03}' for i in range(256)]:
        raise ValueError('Sealed directory contents differ')
    for index in (0, 107, 255):
        dest = work / f'copy-{index}'
        subprocess.run([*selected, f'/Known/f{index:03}', dest], check=True)
        if dest.read_bytes() != payload(index):
            raise ValueError(f'Sealed file {index} differs')
    if hashlib.sha256(image.read_bytes()).digest() != before:
        raise ValueError('Source image changed')
    print('PASS: sealed catalog, file contents and unchanged source image')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--make-image', type=Path, help='keep a new image for use with another reader')
    args = parser.parse_args()
    if args.make_image:
        make_image(args.make_image.resolve())
    else:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            image = work / 'sealed.dmg'
            make_image(image)
            verify(Path(__file__).resolve().parents[1] / 'build/host/ApfsRead', image, work)
