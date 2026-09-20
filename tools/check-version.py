#!/usr/bin/env python3
"""Verify the release version is defined once and reused consistently."""

from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / 'src' / 'version.h'
CPP_FILES = [ROOT / 'src' / 'dlss5-feed.cpp', ROOT / 'src' / 'dlss5-feed32.cpp']
RC_FILE = ROOT / 'src' / 'version.rc'


def read(path: Path) -> str:
    return path.read_text(encoding='utf-8', errors='strict')


def main() -> int:
    if not HEADER.exists():
        print('FAIL: missing src/version.h')
        return 1

    header = read(HEADER)
    version_match = re.search(r'#define\s+FEED_VERSION\s+"([^"]+)"', header)
    if not version_match:
        print('FAIL: FEED_VERSION is not defined in src/version.h')
        return 1
    version = version_match.group(1)

    for path in CPP_FILES:
        cpp = read(path)
        if f'#define FEED_VERSION "{version}"' in cpp:
            continue
        if '#include "version.h"' in cpp and f'FEED_VERSION' in cpp:
            continue
        print(f'FAIL: {path.relative_to(ROOT)} does not use the shared version header correctly')
        return 1

    rc = read(RC_FILE)
    rc_uses_header = '#include "version.h"' in rc and 'FILEVERSION    FEED_VERSION_NUM' in rc and 'PRODUCTVERSION FEED_VERSION_NUM' in rc
    if not rc_uses_header:
        print('FAIL: src/version.rc does not include the shared version metadata')
        return 1
    if 'VALUE "FileVersion",      FEED_VERSION' not in rc and f'VALUE "FileVersion",      "{version}"' not in rc:
        print('FAIL: src/version.rc file version does not match FEED_VERSION')
        return 1
    if 'VALUE "ProductVersion",   FEED_VERSION' not in rc and f'VALUE "ProductVersion",   "{version}"' not in rc:
        print('FAIL: src/version.rc product version does not match FEED_VERSION')
        return 1

    print(f'PASS: FEED_VERSION {version} is shared and consistent across release files')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
