"""Copies the System and script files into the app bundle's Game folder, with Game/manifest.txt listing them.

SDLLaunch copies them to the app's Documents folder on first run and after each update, as the engine
writes next to its files and the signed bundle is read-only. The layout and phone defaults are Android's.
"""
import argparse
import hashlib
import os
import shutil
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'Android'))
from pack_assets import OVERRIDES  # noqa: E402
from pack_common import collect, read  # noqa: E402


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--out', required=True, help='the Game folder in the .app')
    p.add_argument('--source', required=True, help='repository root')
    p.add_argument('--scripts', required=True, help='folder with the compiled .u and .ucl files')
    a = p.parse_args()

    files = collect(a.source, a.scripts, 'UT2004/')
    shutil.rmtree(a.out, ignore_errors=True)
    stamp = hashlib.sha256()
    for src, rel in files:
        data = read(src, rel, OVERRIDES)
        stamp.update(('%s %d\n' % (rel, len(data))).encode())
        stamp.update(data)
        out = os.path.join(a.out, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out, 'wb') as f:
            f.write(data)

    # The content's digest, so an update with changed files copies them again and an unchanged one doesn't.
    with open(os.path.join(a.out, 'manifest.txt'), 'w', newline='\n') as f:
        f.write(stamp.hexdigest() + '\n' + ''.join(rel + '\n' for _, rel in files))
    print('Packed %d files into %s' % (len(files), a.out))


if __name__ == '__main__':
    main()
