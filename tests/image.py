#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 John Knipper
"""Build a disposable APFS fixture with macOS and read it through ApfsRead."""
import hashlib
from pathlib import Path
import plistlib
import re
import subprocess
import sys
import tempfile


def capture(*args):
    return subprocess.check_output(args)


def verify(reader, work):
    image = work / 'fixture.dmg'
    payload = bytes(range(256)) * 1537 + b'APFS'
    subprocess.run(['hdiutil', 'create', '-size', '32m', '-layout', 'GPTSPUD',
                    '-fs', 'APFS', '-volname', 'ApfsReadTest', image], check=True)
    devices = plistlib.loads(capture('hdiutil', 'attach', '-nobrowse', '-plist', image))
    entities = devices['system-entities']
    whole = next(e['dev-entry'] for e in entities if e.get('content-hint') == 'GUID_partition_scheme')
    try:
        volume = next(e for e in entities if e.get('volume-kind') == 'apfs')
        info = plistlib.loads(capture('diskutil', 'info', '-plist', volume['dev-entry']))
        volume_id = info['VolumeUUID'].lower()
        directory = Path(volume['mount-point']) / 'Known'
        directory.mkdir()
        (directory / 'payload.bin').write_bytes(payload)
    finally:
        subprocess.run(['hdiutil', 'detach', whole], check=True)

    before = hashlib.sha256(image.read_bytes()).digest()
    cmd = [reader, '--image', image, '512']
    inventory = capture(*cmd).decode()
    match = re.search(r'^partition=(\d+) volume=(\d+) uuid=([0-9a-f-]+) locked=0 name=ApfsReadTest$',
                      inventory, re.MULTILINE)
    if not match or match[3] != volume_id:
        raise ValueError(f'Unexpected volume inventory: {inventory}')
    selected = cmd + [match[1], match[2]]
    if b'payload.bin' not in capture(*selected, '/Known').splitlines():
        raise ValueError('File missing from APFS directory')
    target = work / 'copy.bin'
    subprocess.run([*selected, '/Known/payload.bin', target], check=True)
    if target.read_bytes() != payload:
        raise ValueError('Extracted file differs from macOS source')
    failed = subprocess.run([*selected, '/missing'], capture_output=True)
    if failed.returncode == 0 or hashlib.sha256(image.read_bytes()).digest() != before:
        raise ValueError('Missing-path or read-only image check failed')
    print('PASS: volume UUID, directory, file contents and unchanged source image')


if __name__ == '__main__':
    with tempfile.TemporaryDirectory() as directory:
        verify(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/host/ApfsRead').resolve(),
               Path(directory))
