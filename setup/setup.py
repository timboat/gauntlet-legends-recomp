"""Gauntlet Legends: Recompiled -- the one-click setup that SETUP.bat runs.

From your own Gauntlet Legends (USA) ROM it:
  1. checks the ROM and keeps a big-endian (.z64) copy in rom/
  2. downloads the pinned versions of N64Recomp, N64ModernRuntime and RT64 into deps/ and applies
     this repo's patches to them (patches/)
  3. finds or downloads a compiler (setup/toolchain.py)
  4. recompiles the game's code from the ROM (N64Recomp + RSPRecomp) into recomp/
  5. builds the PC game, port/build/GauntletRecomp.exe
  6. optionally builds the PS Vita version (setup/vita.py) into vita_files/

Nothing from the game is in this repo: the ROM, the code made from it and everything built from that
stay on your PC. Finished steps are skipped when you run it again; after an update (git pull or a new
ZIP) it re-applies changed patches and rebuilds what changed.

usage: setup.py [ROM file] [--vita | --no-vita | --vita-only]
"""
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netcert      # noqa: E402
import toolchain    # noqa: E402
from toolchain import ROOT, ask, say, fwd   # noqa: E402

netcert.install()   # HTTPS on a fresh Windows: let Windows fetch missing root certificates

ROM_SHA256 = '1cc9cb70686b4785c46fff337ded30a2e1f83f2d63768ad8041e978ad0379ec9'
ROM_NAME = 'Gauntlet Legends (USA).z64'
ROM_DIR = os.path.join(ROOT, 'rom')
ROM_PATH = os.path.join(ROM_DIR, ROM_NAME)
DEPS = os.path.join(ROOT, 'deps')
RECOMP = os.path.join(ROOT, 'recomp')
PATCHES = os.path.join(ROOT, 'patches')
PC_BUILD = os.path.join(ROOT, 'port', 'build')

# The upstream projects, at the commits this port was made with. "patches" are applied in the named
# (sub)folder; "pins" check out another commit of a submodule first.
REPOS = [
    dict(name='N64Recomp', url='https://github.com/N64Recomp/N64Recomp.git',
         commit='ffb39cdad1da5de07eaaa48bd1db4a89a7986771',
         patches=[('', 'N64Recomp.patch')]),
    dict(name='N64ModernRuntime', url='https://github.com/N64Recomp/N64ModernRuntime.git',
         commit='df7e820d8c55e4fcb4616c210cbb2c01b25cd48c',
         pins=[('N64Recomp/lib/fmt', '407c905e45ad75fc29bf0f9bb7c5c2fd3475976f')],   # fmt 12.1.0: newer clang
         patches=[('', 'N64ModernRuntime.patch'), ('N64Recomp', 'N64ModernRuntime-N64Recomp.patch')]),
    dict(name='rt64', url='https://github.com/rt64/rt64.git',
         commit='23cab603c4f9f4a8b369b38e036f1aa484603878',
         patches=[('', 'rt64.patch'), ('src/contrib/mupen64plus-win32-deps', 'rt64-mupen64plus-win32-deps.patch')]),
]
SDL2_URL = 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-VC.zip'
SDL2_SHA256 = '8c91d91e5bcb997d062ec2b553c53832ebf95654d4aa35e8c02a954d4ce752ae'
SDL2_DIR = os.path.join(DEPS, 'sdl2', 'SDL2-2.30.9')


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def step(n, text):
    say()
    say(f'[{n}/6] {text}')


# --- 1. the ROM ----------------------------------------------------------------------------------

def to_z64(data):
    """.z64 (big-endian, as the cartridge), .v64 (16-bit byte-swapped) or .n64 (little-endian) -> .z64"""
    magic = data[:4]
    if magic == b'\x80\x37\x12\x40':
        return data
    if magic == b'\x37\x80\x40\x12':
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b)
    if magic == b'\x40\x12\x37\x80':
        b = bytearray(data)
        b[0::4], b[1::4], b[2::4], b[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(b)
    return None


def read_rom(path):
    """The ROM's bytes from a .z64/.v64/.n64 file or a .zip holding one; None if it is not a ROM."""
    try:
        if zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as z:
                for n in z.namelist():
                    if n.lower().endswith(('.z64', '.v64', '.n64')):
                        return to_z64(z.read(n))
            return None
        if os.path.getsize(path) > 128 * 1024 * 1024:
            return None
        with open(path, 'rb') as f:
            return to_z64(f.read())
    except (OSError, zipfile.BadZipFile):
        return None


def rom_candidates(arg):
    if arg:
        yield arg
    for d in (ROM_DIR, ROOT):
        if os.path.isdir(d):
            for n in sorted(os.listdir(d)):
                if n.lower().endswith(('.z64', '.v64', '.n64', '.zip')):
                    yield os.path.join(d, n)


def get_rom(arg):
    if os.path.isfile(ROM_PATH) and sha256_file(ROM_PATH) == ROM_SHA256:
        say(f'  {ROM_PATH}: OK')
        return True
    tried = []
    while True:
        for p in rom_candidates(arg):
            p = os.path.abspath(p.strip().strip('"'))
            if p in tried or not os.path.isfile(p) or os.path.abspath(p) == os.path.abspath(ROM_PATH):
                continue
            tried.append(p)
            data = read_rom(p)
            if data is None:
                say(f'  {p}: not an N64 ROM')
                continue
            if hashlib.sha256(data).hexdigest() != ROM_SHA256:
                say(f'  {p}: an N64 ROM, but not Gauntlet Legends (USA) -- only that version is supported')
                continue
            os.makedirs(ROM_DIR, exist_ok=True)
            with open(ROM_PATH + '.part', 'wb') as f:
                f.write(data)
            os.replace(ROM_PATH + '.part', ROM_PATH)
            say(f'  {p}: Gauntlet Legends (USA), copied to rom\\{ROM_NAME}')
            return True
        say('  Your Gauntlet Legends (USA) ROM is needed (.z64, .v64, .n64, or a .zip with one in it).')
        say('  Drag the file into this window and press Enter (or put it in the rom folder and press Enter);')
        arg = ask('  just Enter to stop: ')
        if not arg:
            return False


# --- 2. the upstream projects ----------------------------------------------------------------------

def git(*args, cwd=None, check=True, quiet=False):
    cmd = ['git', '-c', 'core.autocrlf=false', '-c', 'advice.detachedHead=false', *args]
    r = subprocess.run(cmd, cwd=cwd, capture_output=quiet, text=True)
    if check and r.returncode:
        if quiet:
            say((r.stdout or '') + (r.stderr or ''))
        raise RuntimeError('git ' + ' '.join(args) + ' failed')
    return r


def retry(action, cleanup=None, tries=4):
    """Downloads from GitHub fail now and then (a dropped connection): wait and try again."""
    import time
    for n in range(1, tries + 1):
        try:
            return action()
        except RuntimeError:
            if n == tries:
                raise RuntimeError('the download kept failing: check the internet connection and run the setup '
                                   'again (it continues where it stopped)')
            say(f'  the download failed; trying again in {15 * n} s ...')
            if cleanup:
                cleanup()
            time.sleep(15 * n)


def patch_key(repo):
    """What the patched checkout should be: the commit, the pins and the patches' contents."""
    h = hashlib.sha256(repo['commit'].encode())
    for sub, c in repo.get('pins', []):
        h.update(f'{sub}@{c}'.encode())
    for sub, p in repo['patches']:
        h.update(sub.encode())
        with open(os.path.join(PATCHES, p), 'rb') as f:
            h.update(f.read())
    return h.hexdigest()


def get_repo(repo):
    dest = os.path.join(DEPS, repo['name'])
    mark = os.path.join(dest, '.gauntlet_patched')
    key = patch_key(repo)
    if os.path.isfile(mark) and open(mark).read().strip() == key:
        say(f'  deps\\{repo["name"]}: OK')
        return
    if not os.path.isdir(os.path.join(dest, '.git')):
        # into <name>.part, renamed when complete; a download cut off part-way is picked up where it stopped
        part = dest + '.part'
        resumable = os.path.isdir(os.path.join(part, '.git')) and not git(
            'rev-parse', '--verify', '--quiet', repo['commit'] + '^{commit}', cwd=part, check=False, quiet=True).returncode
        if not resumable:
            shutil.rmtree(part, ignore_errors=True)
            say(f'  downloading {repo["name"]} ({repo["url"]}) ...')
            retry(lambda: git('clone', '--quiet', repo['url'], part),
                  lambda: shutil.rmtree(part, ignore_errors=True))
        git('checkout', '--quiet', '--force', repo['commit'], cwd=part)
        say(f'  downloading {repo["name"]}\'s submodules ...')
        retry(lambda: git('submodule', 'update', '--quiet', '--init', '--recursive', cwd=part))
        os.replace(part, dest)
    else:
        # an earlier version of the patches (an update): back to the clean commit first
        say(f'  deps\\{repo["name"]}: the patches changed, applying them again')
        if os.path.isfile(mark):
            os.remove(mark)
        git('fetch', '--quiet', 'origin', cwd=dest, check=False)
        git('checkout', '--quiet', '--force', repo['commit'], cwd=dest)
        git('submodule', 'update', '--quiet', '--init', '--recursive', '--force', cwd=dest)
        git('submodule', 'foreach', '--quiet', '--recursive', 'git checkout --quiet -- .', cwd=dest)
    for sub, commit in repo.get('pins', []):
        d = os.path.join(dest, sub)
        if git('cat-file', '-e', commit + '^{commit}', cwd=d, check=False, quiet=True).returncode:
            git('fetch', '--quiet', 'origin', cwd=d)
        git('checkout', '--quiet', commit, cwd=d)
    for sub, p in repo['patches']:
        d = os.path.join(dest, sub) if sub else dest
        r = git('apply', '--ignore-whitespace', '--whitespace=nowarn', os.path.join(PATCHES, p), cwd=d, check=False,
                quiet=True)
        if r.returncode:
            say((r.stdout or '') + (r.stderr or ''))
            raise RuntimeError(f'patches\\{p} does not apply to deps\\{repo["name"]}')
    with open(mark, 'w') as f:
        f.write(key + '\n')
    say(f'  deps\\{repo["name"]}: ready')


def download(url, sha256, what):
    say(f'  downloading {what}: {url}')
    data = urllib.request.urlopen(url, timeout=300).read()
    if hashlib.sha256(data).hexdigest() != sha256:
        raise RuntimeError(f'{what} does not match its checksum')
    return data


def get_sdl2():
    if os.path.isfile(os.path.join(SDL2_DIR, 'lib', 'x64', 'SDL2.dll')):
        say('  deps\\sdl2: OK')
        return
    import io
    data = download(SDL2_URL, SDL2_SHA256, 'SDL2 2.30.9 (open source)')
    part = os.path.join(DEPS, 'sdl2.part')
    shutil.rmtree(part, ignore_errors=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        z.extractall(part)
    shutil.rmtree(os.path.join(DEPS, 'sdl2'), ignore_errors=True)
    os.replace(part, os.path.join(DEPS, 'sdl2'))
    say('  deps\\sdl2: ready')


# --- 4. recompiling ------------------------------------------------------------------------------

def recompile(tc):
    tools_dir = os.path.join(DEPS, 'N64Recomp', 'build')
    n64recomp = os.path.join(tools_dir, 'N64Recomp' + toolchain.EXE)
    rsprecomp = os.path.join(tools_dir, 'RSPRecomp' + toolchain.EXE)
    if not (os.path.isfile(n64recomp) and os.path.isfile(rsprecomp)):
        say('  building N64Recomp and RSPRecomp ...')
        if tc.cmake_build(os.path.join(DEPS, 'N64Recomp'), tools_dir, ['N64RecompCLI', 'RSPRecomp'],
                          build_type='Release'):
            raise RuntimeError('building N64Recomp failed')
    # inputs of the recompiled code: the tools' commit + patches and this repo's recomp/*.toml
    h = hashlib.sha256(patch_key(REPOS[0]).encode())
    for n in ('gauntlet.toml', 'gauntlet.syms.toml', 'audio_ucode.toml'):
        with open(os.path.join(RECOMP, n), 'rb') as f:
            h.update(f.read())
    with open(os.path.join(ROOT, 'tools', 'rsp_prof_marks.py'), 'rb') as f:
        h.update(f.read())
    key = h.hexdigest()
    mark = os.path.join(RECOMP, '.recompiled')
    if os.path.isfile(mark) and open(mark).read().strip() == key:
        say('  recomp\\RecompiledFuncs: OK')
        return
    if os.path.isfile(mark):
        os.remove(mark)
    say('  making the code image from the ROM ...')
    py = sys.executable
    code_bin = os.path.join(RECOMP, 'gauntlet_code.bin')
    if subprocess.run([py, os.path.join(ROOT, 'tools', 'make_code_bin.py'), ROM_PATH, code_bin]).returncode:
        raise RuntimeError('making the code image failed')
    say('  recompiling the game code (N64Recomp) ...')
    out = os.path.join(RECOMP, 'RecompiledFuncs')
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(RECOMP, 'rsp'), exist_ok=True)
    with open(os.path.join(RECOMP, 'recomp.log'), 'w') as log:
        if subprocess.run([n64recomp, 'gauntlet.toml'], cwd=RECOMP, stdout=log, stderr=subprocess.STDOUT).returncode:
            raise RuntimeError('N64Recomp failed (see recomp\\recomp.log)')
        say('  recompiling the audio microcode (RSPRecomp) ...')
        if subprocess.run([rsprecomp, 'audio_ucode.toml'], cwd=RECOMP, stdout=log, stderr=subprocess.STDOUT).returncode:
            raise RuntimeError('RSPRecomp failed (see recomp\\recomp.log)')
    ucode = os.path.join(RECOMP, 'rsp', 'audio_ucode.cpp')
    subprocess.run([py, os.path.join(ROOT, 'tools', 'rsp_prof_marks.py'), ucode, ucode], check=True)
    n = len([f for f in os.listdir(out) if f.endswith('.c')])
    with open(mark, 'w') as f:
        f.write(key + '\n')
    say(f'  recomp\\RecompiledFuncs: {n} files')


# --- 5. the PC game ------------------------------------------------------------------------------

def build_pc(tc):
    say('  building GauntletRecomp.exe (the first build takes a while; later ones only what changed) ...')
    if tc.cmake_build(os.path.join(ROOT, 'port'), PC_BUILD, ['GauntletRecomp'], build_type='RelWithDebInfo'):
        raise RuntimeError('building the game failed (see the messages above)')
    say(f'  {os.path.join(PC_BUILD, "GauntletRecomp.exe")}: built')


def main():
    toolchain.no_quickedit()
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    flags = {a for a in sys.argv[1:] if a.startswith('--')}
    vita_only = '--vita-only' in flags
    say('Gauntlet Legends: Recompiled - setup')
    try:
        step(1, 'The ROM')
        if not get_rom(args[0] if args else None):
            return 1
        step(2, 'Downloading the recompiler, the N64 runtime and the renderer (pinned versions + patches)')
        os.makedirs(DEPS, exist_ok=True)
        for r in REPOS:
            get_repo(r)
        if not vita_only:
            get_sdl2()
        step(3, 'Compiler')
        tc = toolchain.find()
        if tc is None:
            return 1
        step(4, 'Recompiling the game code from the ROM')
        recompile(tc)
        if vita_only:
            say('\n[5/6] The PC game: skipped (--vita-only)')
        else:
            step(5, 'Building the PC game')
            build_pc(tc)
        step(6, 'PS Vita version')
        want_vita = vita_only or '--vita' in flags
        if not want_vita and '--no-vita' not in flags:
            want_vita = ask('  Also build the PS Vita version (a .vpk for a hacked Vita, about 600 MB of tools)? '
                            '[y/N] ') in ('y', 'yes')
        if want_vita:
            import vita
            if not vita.build(ROM_PATH):
                return 1
        else:
            say('  skipped (BUILD_VITA.bat builds it later)')
    except (RuntimeError, OSError) as e:
        say(f'\n  {e}')
        return 1
    say()
    if not vita_only:
        say('All done. Start the game with PLAY.bat.')
    else:
        say('All done.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
