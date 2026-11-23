"""The PS Vita version: VitaSDK (pinned, downloaded into deps/vitasdk) -> vita/build/gauntlet.vpk, and a
vita_files/ folder laid out the way it goes on the memory card.

The recompiled code in recomp/ must exist (setup.py makes it first). The VPK holds that code, so it is
built on your PC and never shared.
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

import toolchain
from toolchain import ROOT, fwd, say

VITASDK = os.path.join(ROOT, 'deps', 'vitasdk')
SDK_URL = ('https://github.com/vitasdk/autobuilds/releases/download/sdk-snapshot-20260825.611.1/'
           'vitasdk-x86_64-w64-mingw32-2026-08-25_13-18-42.tar.bz2')
SDK_SHA256 = 'c5764ffd2b141275393eefaebaa3fa1b33e2baddaf32e440eb5b8953370e32ea'
PKG_BASE = 'https://github.com/vitasdk/vitasdk-autobuild/releases/download/packages-2026.08-snapshot-20261017.59.1/'
PACKAGES = [   # the libraries the Vita build links (vita/CMakeLists.txt)
    ('libvita2d-0.0.0.r188.ga8f15ab-1-vita.pkg.tar.xz',
     'd604b050e6e445e4019258a75806f24fb325e8abb8dfdc48f6c1fbaa7346fd43'),
    ('vitaShaRK-1.7-1-vita.pkg.tar.xz', 'd41879db12c8e1434dc41d9936b14701a9d064b24b8b1c56a74680099db632a9'),
    ('SceShaccCgExt-1.0.1-2-vita.pkg.tar.xz', '535bd8d6845d2ec6e9924efba98c1b8cb7432fffdc84ce07dc164442d5cdd148'),
    ('taihen-0.11-1-vita.pkg.tar.xz', '93d2075b6b61a164224a2aad651d3d6787f31f3a19c3e2bada7c99844f9fae90'),
]
BUILD = os.path.join(ROOT, 'vita', 'build')
OUT = os.path.join(ROOT, 'vita_files')


def fetch(url, sha256, what):
    say(f'  downloading {what}: {url}')
    h = hashlib.sha256()
    buf = io.BytesIO()
    with urllib.request.urlopen(url, timeout=300) as r:
        for b in iter(lambda: r.read(1 << 20), b''):
            h.update(b)
            buf.write(b)
    if h.hexdigest() != sha256:
        raise RuntimeError(f'{what} does not match its checksum')
    buf.seek(0)
    return buf


def safe_members(t, strip):
    for m in t:
        parts = m.name.replace('\\', '/').split('/')[strip:]
        if not parts or not parts[0] or '..' in parts or m.name.startswith(('/', '\\')) or parts[0].startswith('.'):
            continue
        if not (m.isfile() or m.isdir()):
            continue
        yield m, parts


def get_sdk():
    key = SDK_SHA256 + ''.join(s for _, s in PACKAGES)
    mark = os.path.join(VITASDK, '.gl_sdk')
    if os.path.isfile(mark) and open(mark).read().strip() == key:
        say('  deps\\vitasdk: OK')
        return
    part = VITASDK + '.part'
    shutil.rmtree(part, ignore_errors=True)
    data = fetch(SDK_URL, SDK_SHA256, 'VitaSDK (open source, 116 MB)')
    say('  unpacking VitaSDK ...')
    with tarfile.open(fileobj=data, mode='r:bz2') as t:
        for m, parts in safe_members(t, 1):   # vitasdk/...
            out = os.path.join(part, *parts)
            if m.isdir():
                os.makedirs(out, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with t.extractfile(m) as src, open(out, 'wb') as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
    for name, sha in PACKAGES:
        data = fetch(PKG_BASE + name, sha, name.split('-')[0])
        with tarfile.open(fileobj=data, mode='r:xz') as t:
            for m, parts in safe_members(t, 0):   # arm-vita-eabi/... (and .PKGINFO etc., skipped)
                out = os.path.join(part, *parts)
                if m.isdir():
                    os.makedirs(out, exist_ok=True)
                else:
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with t.extractfile(m) as src, open(out, 'wb') as dst:
                        shutil.copyfileobj(src, dst, 1 << 20)
    with open(os.path.join(part, '.gl_sdk'), 'w') as f:
        f.write(key + '\n')
    shutil.rmtree(VITASDK, ignore_errors=True)
    os.replace(part, VITASDK)
    say('  deps\\vitasdk: ready')


def build(rom_path):
    try:
        get_sdk()
    except (OSError, RuntimeError, tarfile.TarError) as e:
        say(f'  VitaSDK: {e}')
        return False
    cmake, ninja = toolchain.pip_tool('cmake'), toolchain.pip_tool('ninja')
    if not cmake or not ninja:
        say('  CMake / Ninja missing: run SETUP.bat (it installs them into .venv).')
        return False
    if not os.path.isfile(os.path.join(ROOT, 'vita', 'sce_sys', 'icon0.png')):
        say('  making the bubble icon and LiveArea images ...')
        if subprocess.run([sys.executable, os.path.join(ROOT, 'vita', 'make_livearea.py')]).returncode:
            return False
    env = dict(os.environ, VITASDK=fwd(VITASDK))
    env['PATH'] = os.path.join(VITASDK, 'bin') + os.pathsep + env.get('PATH', '')
    if not os.path.isfile(os.path.join(BUILD, 'build.ninja')):
        cfg = [cmake, '-S', fwd(os.path.join(ROOT, 'vita')), '-B', fwd(BUILD), '-G', 'Ninja',
               f'-DCMAKE_MAKE_PROGRAM={fwd(ninja)}', '-DCMAKE_BUILD_TYPE=Release',
               f'-DCMAKE_TOOLCHAIN_FILE={fwd(os.path.join(VITASDK, "share", "vita.toolchain.cmake"))}']
        if toolchain.run_quiet(cfg, env=env, what='configuring (Vita)', log_name='build_vita.txt'):
            return False
    say('  building gauntlet.vpk (the first build takes a while) ...')
    if toolchain.run_quiet([ninja, '-C', BUILD], env=env, what='compiling (Vita)', log_name='build_vita.txt'):
        say('  building the Vita version failed (see above)')
        return False
    vpk = os.path.join(BUILD, 'gauntlet.vpk')
    data = os.path.join(OUT, 'ux0', 'data', 'GauntletLegends')
    os.makedirs(data, exist_ok=True)
    shutil.copyfile(vpk, os.path.join(OUT, 'GauntletLegends.vpk'))
    shutil.copyfile(rom_path, os.path.join(data, 'rom.z64'))
    with open(os.path.join(OUT, 'README.txt'), 'w') as f:
        f.write(VITA_README)
    say(f'  {OUT}: GauntletLegends.vpk + ux0\\data\\GauntletLegends\\rom.z64 (see README.txt there)')
    return True


VITA_README = """Gauntlet Legends: Recompiled for PS Vita
========================================

Needs a Vita (or PS TV) with HENkaku/Enso and VitaShell.

1. Copy GauntletLegends.vpk to the Vita (for example to ux0:vpk/) and install it with VitaShell.
2. Copy the ux0\\data\\GauntletLegends folder from here to ux0:data/ on the Vita, so the ROM ends up at
   ux0:data/GauntletLegends/rom.z64. That copies only rom.z64; never delete or replace the saves folder
   there on the Vita (your Controller Pak saves).
3. Start Gauntlet Legends from the LiveArea.

Optional: with libshacccg.suprx installed (ur0:data/, made by ShaRKF00D) cut-out textures such as
fences and foliage are drawn on the GPU (one shader is compiled and cached on first start); without it
the game still runs, using a fallback for those.

The VPK and the ROM copy are made from your own ROM: keep them to yourself.
"""
