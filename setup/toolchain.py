"""The C/C++ compiler for the PC build and for the recompiler tools (Windows).

Uses, in this order:
  - clang-cl + lld-link from an installed LLVM 17 or newer (LLVM_DIR, deps/llvm, C:\\Program Files\\LLVM,
    Visual Studio's own Clang component), else a portable LLVM downloaded into deps/llvm;
  - the Windows SDK and C++ libraries from Visual Studio 2022 (or its Build Tools) when installed, else
    Microsoft's C runtime + Windows SDK fetched by xwin into deps/xwin (Microsoft's license applies; xwin
    shows it). Then the game is linked with the static C runtime.
CMake and Ninja come from the Python environment SETUP.bat makes.

Test switches: GL_TEST_NO_VS=1 / GL_TEST_NO_LLVM=1 act as if Visual Studio / LLVM were not installed,
GL_ASSUME_YES=1 answers every [Y/n] question with the default, GL_BUILD_VERBOSE=1 shows the compilers' output.
"""
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = '.exe' if os.name == 'nt' else ''
MIN_CLANG = 17
LLVM_VERSION = '23.1.4'
LLVM_URL = ('https://github.com/llvm/llvm-project/releases/download/llvmorg-{v}/'
            'clang+llvm-{v}-x86_64-pc-windows-msvc.tar.zst')
LLVM_SHA256 = '1a3f2f998e230b5791cc4e2bfe11a1f7c07fe42c96ff44f227c9db0bedfaf091'
LLVM_TOOLS = ('clang-cl', 'lld-link', 'llvm-lib', 'llvm-rc', 'llvm-mt')
XWIN_DIR = os.path.join(ROOT, 'deps', 'xwin')
XWIN_VERSION = '0.10.0'
XWIN_URL = 'https://github.com/Jake-Shadle/xwin/releases/download/{v}/xwin-{v}-x86_64-pc-windows-msvc.tar.gz'
XWIN_LICENSE = 'https://go.microsoft.com/fwlink/?LinkId=2086102'
VCREDIST_URL = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'
BLOCKED_WINERRORS = (4551, 1260)   # application control policy (Smart App Control / WDAC), group policy


def say(msg=''):
    print(msg, flush=True)


def ask(prompt):
    if os.environ.get('GL_ASSUME_YES') == '1' and ('[Y/n]' in prompt or '[y/N]' in prompt):
        say(prompt + '(default: GL_ASSUME_YES)')
        return ''
    try:
        return input(prompt).strip().lower()
    except EOFError:
        return ''


def fwd(p):
    return p.replace('\\', '/')


def no_quickedit():
    """With QuickEdit on (the Windows default) one stray click in the console pauses the setup until a key
    is pressed. Turn it off for this window only."""
    if os.name != 'nt':
        return
    try:
        import ctypes
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-10)
        mode = ctypes.c_uint32()
        if h and k.GetConsoleMode(h, ctypes.byref(mode)):
            k.SetConsoleMode(h, (mode.value & ~0x40) | 0x80)
    except (OSError, AttributeError, ValueError):
        pass


def blocked(e, what):
    if getattr(e, 'winerror', None) not in BLOCKED_WINERRORS:
        return False
    say(f'  Windows blocked {what}: Smart App Control (or another app control policy) does not let unsigned')
    say('  programs run, and this port is built on your PC, so its tools and the game are unsigned. Turn it off')
    say('  in Windows Security -> App & browser control -> Smart App Control, then run the setup again.')
    return True


# --- finding things ------------------------------------------------------------------------------

def find_vs():
    if os.environ.get('GL_TEST_NO_VS') == '1':
        return None, None
    pf86 = os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')
    vswhere = os.path.join(pf86, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    if os.path.isfile(vswhere):
        r = subprocess.run([vswhere, '-latest', '-products', '*', '-requires',
                            'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                           capture_output=True, text=True)
        for line in r.stdout.splitlines():
            bat = os.path.join(line.strip(), 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
            if os.path.isfile(bat):
                return line.strip(), bat
    return None, None


def clang_version(bindir):
    try:
        out = subprocess.run([os.path.join(bindir, 'clang-cl' + EXE), '--version'], capture_output=True,
                             text=True).stdout
    except OSError as e:
        if blocked(e, os.path.join(bindir, 'clang-cl' + EXE)):
            sys.exit(1)
        return 0, ''
    m = re.search(r'version (\d+)\.', out)
    return (int(m.group(1)) if m else 0), (out.splitlines()[0] if out else '')


def find_llvm(vs_path):
    cands = glob.glob(os.path.join(ROOT, 'deps', 'llvm', '*', 'bin'))
    if os.environ.get('GL_TEST_NO_LLVM') != '1':
        if os.environ.get('LLVM_DIR'):
            cands += [os.environ['LLVM_DIR'], os.path.join(os.environ['LLVM_DIR'], 'bin')]
        cands.append(os.path.join(os.environ.get('ProgramFiles', r'C:\Program Files'), 'LLVM', 'bin'))
        if vs_path:
            cands.append(os.path.join(vs_path, 'VC', 'Tools', 'Llvm', 'x64', 'bin'))
        w = shutil.which('clang-cl')
        if w:
            cands.append(os.path.dirname(w))
    best = None
    for c in cands:
        if all(os.path.isfile(os.path.join(c, t + EXE)) for t in ('clang-cl', 'lld-link', 'llvm-lib')):
            v, text = clang_version(c)
            if v >= MIN_CLANG and (best is None or v > best[0]):
                best = (v, c, text)
    return best


def pip_tool(name):
    try:
        if name == 'cmake':
            import cmake
            p = os.path.join(cmake.CMAKE_BIN_DIR, 'cmake' + EXE)
        else:
            import ninja
            p = os.path.join(ninja.BIN_DIR, 'ninja' + EXE)
        if os.path.isfile(p):
            return p
    except (ImportError, AttributeError):
        pass
    return shutil.which(name, path=os.path.dirname(sys.executable)) or shutil.which(name)


# --- downloads -----------------------------------------------------------------------------------

def download_llvm():
    """LLVM's own Windows release (.tar.zst, pinned sha256); only the tools the build uses and clang's
    headers are unpacked (~250 MB), into deps/llvm/<release folder>."""
    try:
        import zstandard
    except ImportError:
        say('  the zstandard package is missing: run SETUP.bat (it installs it).')
        return False
    url = LLVM_URL.format(v=LLVM_VERSION)
    say(f'  downloading LLVM {LLVM_VERSION} (open source, 280 MB): {url}')
    os.makedirs(os.path.join(ROOT, 'deps'), exist_ok=True)
    part = os.path.join(ROOT, 'deps', 'llvm.tar.zst.part')
    h = hashlib.sha256()
    try:
        with urllib.request.urlopen(url, timeout=120) as r, open(part, 'wb') as f:
            for b in iter(lambda: r.read(1 << 20), b''):
                h.update(b)
                f.write(b)
    except OSError as e:
        say(f'  download failed: {e}')
        return False
    if h.hexdigest() != LLVM_SHA256:
        os.remove(part)
        say('  the LLVM download does not match its checksum; not using it')
        return False
    say('  unpacking clang-cl, lld-link, llvm-lib, llvm-rc, llvm-mt and clang\'s headers ...')
    final = os.path.join(ROOT, 'deps', 'llvm')
    dest = final + '.part'
    shutil.rmtree(dest, ignore_errors=True)
    want = {f'bin/{t}.exe' for t in LLVM_TOOLS}
    try:
        with open(part, 'rb') as f:
            rd = zstandard.ZstdDecompressor(max_window_size=2 ** 31).stream_reader(f)
            with tarfile.open(fileobj=rd, mode='r|') as t:
                for m in t:
                    parts = m.name.replace('\\', '/').split('/')
                    if len(parts) < 2 or '..' in parts or m.name.startswith(('/', '\\')):
                        continue
                    rel = '/'.join(parts[1:])
                    if not (rel in want or rel.startswith('lib/clang/') or rel == 'LICENSE.TXT'):
                        continue
                    if '/finclude/' in rel:
                        continue
                    out = os.path.join(dest, *parts)
                    if m.isdir():
                        os.makedirs(out, exist_ok=True)
                    elif m.isfile():
                        os.makedirs(os.path.dirname(out), exist_ok=True)
                        with t.extractfile(m) as src, open(out, 'wb') as dst:
                            shutil.copyfileobj(src, dst, 1 << 20)
    except (OSError, tarfile.TarError, zstandard.ZstdError) as e:
        say(f'  unpacking LLVM failed: {e}')
        if isinstance(e, FileNotFoundError) and len(ROOT) > 90:
            say(f'  The folder path is long ({len(ROOT)} characters) and Windows limits paths to 260: move the')
            say('  folder somewhere short, such as C:\\Games\\GauntletLegends, and run the setup again.')
        return False
    finally:
        if os.path.isfile(part):
            os.remove(part)
    shutil.rmtree(final, ignore_errors=True)
    os.replace(dest, final)
    return True


def vc_runtime_ok():
    """xwin.exe and the DirectX shader compiler DLLs need Microsoft's Visual C++ runtime, which most PCs have."""
    sysdir = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32')
    if all(os.path.isfile(os.path.join(sysdir, d)) for d in ('vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll')):
        return True
    say('  The Microsoft Visual C++ runtime is not installed on this PC.')
    if ask('  Install it now (Microsoft\'s own installer, about 25 MB; Windows asks for permission)? [Y/n] ') == 'n':
        say(f'  Install it from {VCREDIST_URL} and run the setup again.')
        return False
    if shutil.which('winget') and subprocess.run(
            ['winget', 'install', '-e', '--id', 'Microsoft.VCRedist.2015+.x64', '--accept-package-agreements',
             '--accept-source-agreements']).returncode == 0:
        return True
    exe = os.path.join(tempfile.gettempdir(), 'vc_redist.x64.exe')
    try:
        urllib.request.urlretrieve(VCREDIST_URL, exe)
    except OSError as e:
        say(f'  download failed ({e}); install it from {VCREDIST_URL} and run the setup again.')
        return False
    # only run it when Microsoft signed it
    sig = subprocess.run(['powershell', '-NoProfile', '-Command',
                          f"$s = Get-AuthenticodeSignature -LiteralPath '{exe}'; "
                          "if ($s.Status -eq 'Valid' -and $s.SignerCertificate.Subject -like '*O=Microsoft Corporation*') "
                          "{ exit 0 } else { exit 1 }"])
    if sig.returncode:
        say(f'  the download is not signed by Microsoft; install it from {VCREDIST_URL} by hand.')
        return False
    return subprocess.run([exe, '/install', '/passive', '/norestart']).returncode in (0, 1638, 3010)


def xwin_sdk():
    """Microsoft's C runtime + Windows SDK in deps/xwin, fetched once by xwin (~1 GB download)."""
    mark = os.path.join(XWIN_DIR, '.gl_done')
    if os.path.isfile(mark):
        return XWIN_DIR
    say('  The game is built against Microsoft\'s C runtime and Windows SDK. They are not part of this repo:')
    say('  xwin (an open-source tool) downloads them from Microsoft to this PC, once. Microsoft allows that')
    say(f'  under its license ({XWIN_LICENSE}); xwin asks you to accept it.')
    if not vc_runtime_ok():
        return None
    xwin = os.path.join(ROOT, 'deps', 'xwin-bin', 'xwin' + EXE)
    if not os.path.isfile(xwin):
        url = XWIN_URL.format(v=XWIN_VERSION)
        say(f'  downloading xwin {XWIN_VERSION} (open source, ~3 MB): {url}')
        try:
            data = urllib.request.urlopen(url, timeout=120).read()
            want = urllib.request.urlopen(url + '.sha256', timeout=60).read().decode().split()[0].lower()
        except OSError as e:
            say(f'  download failed: {e}')
            return None
        if hashlib.sha256(data).hexdigest() != want:
            say('  xwin download does not match its sha256; not using it')
            return None
        os.makedirs(os.path.dirname(xwin), exist_ok=True)
        fd, tgz = tempfile.mkstemp(suffix='.tar.gz')
        with os.fdopen(fd, 'wb') as f:
            f.write(data)
        try:
            with tarfile.open(tgz) as t:
                m = next(m for m in t.getmembers() if m.isfile() and os.path.basename(m.name) == 'xwin' + EXE)
                with t.extractfile(m) as src, open(xwin, 'wb') as dst:
                    shutil.copyfileobj(src, dst)
        finally:
            os.remove(tgz)
    cache = os.path.join(ROOT, 'deps', 'xwin-cache')
    cmd = [xwin, '--cache-dir', cache, '--arch', 'x86_64', '--variant', 'desktop', '--http-retry', '3']
    if os.environ.get('XWIN_ACCEPT_LICENSE') == 'true' or os.environ.get('GL_ASSUME_YES') == '1':
        cmd.append('--accept-license')
    cmd += ['splat', '--output', XWIN_DIR, '--use-winsysroot-style', '--preserve-ms-arch-notation',
            '--disable-symlinks']
    shutil.rmtree(XWIN_DIR, ignore_errors=True)
    say('  > xwin splat (downloading the Microsoft CRT + SDK, a few minutes)')
    try:
        rc = subprocess.run(cmd).returncode
    except OSError as e:
        if not blocked(e, xwin):
            say(f'  could not run {xwin}: {e}')
        return None
    if rc:
        say('  xwin failed (see above); run the setup again to retry.')
        return None
    shutil.rmtree(cache, ignore_errors=True)
    inc, lib, _ = xwin_paths(XWIN_DIR)
    missing = [p for p in inc + lib if not os.path.isdir(p)]
    if missing:
        say('  xwin output is missing ' + ', '.join(missing))
        return None
    with open(mark, 'w') as f:
        f.write(f'xwin {XWIN_VERSION}\n')
    return XWIN_DIR


def xwin_paths(sdk):
    def newest(pattern):
        found = sorted(glob.glob(os.path.join(sdk, *pattern)),
                       key=lambda p: [int(x) for x in re.findall(r'\d+', os.path.basename(p))])
        return found[-1] if found else os.path.join(sdk, *pattern).replace('*', '?')
    vc = newest(('VC', 'Tools', 'MSVC', '*'))
    inc_kit = newest(('Windows Kits', '10', 'Include', '*'))
    lib_kit = newest(('Windows Kits', '10', 'Lib', '*'))
    inc = [os.path.join(vc, 'include')] + [os.path.join(inc_kit, d) for d in ('ucrt', 'um', 'shared', 'winrt')]
    lib = [os.path.join(vc, 'lib', 'x64')] + [os.path.join(lib_kit, d, 'x64') for d in ('um', 'ucrt')]
    m = re.match(r'14\.(\d+)', os.path.basename(vc))
    return inc, lib, f'19.{m.group(1)}' if m else '19.40'


def vcvars_env(vcvars):
    """The environment vcvars64.bat sets up (INCLUDE, LIB, PATH to the Windows SDK tools)."""
    r = subprocess.run(f'"{vcvars}" >nul && set', shell=True, capture_output=True, text=True)
    env = {}
    for line in r.stdout.splitlines():
        k, sep, v = line.partition('=')
        if sep:
            env[k] = v
    return env if 'INCLUDE' in env else None


# --- building ------------------------------------------------------------------------------------

def run_quiet(cmd, env=None, what='compiling', log_name='build_last.txt'):
    """One progress line from ninja's [n/N] lines instead of the compilers' (harmless) warnings; errors are
    shown in full. Everything goes to logs/<log_name>."""
    if os.environ.get('GL_BUILD_VERBOSE') == '1':
        return subprocess.run(cmd, env=env).returncode
    os.makedirs(os.path.join(ROOT, 'logs'), exist_ok=True)
    logp = os.path.join(ROOT, 'logs', log_name)
    t0 = time.time()
    state = {'line': f'  {what} ...', 'shown': False}
    lock = threading.Lock()

    def show(final=False):
        el = int(time.time() - t0)
        text = f"{state['line']}  -  {el // 60}:{el % 60:02d}"
        sys.stdout.write('\r' + text.ljust(78) + ('\n' if final else ''))
        sys.stdout.flush()
        state['shown'] = not final

    def above(text):
        with lock:
            if state['shown']:
                sys.stdout.write('\r' + ' ' * 79 + '\r')
            sys.stdout.write(text.rstrip('\n') + '\n')
            show()

    done = threading.Event()

    def tick():
        while not done.wait(1.0):
            with lock:
                show()

    try:
        p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except OSError as e:
        if not blocked(e, cmd[0]):
            say(f'  could not run {cmd[0]}: {e}')
        return 1
    th = threading.Thread(target=tick, daemon=True)
    th.start()
    errors = 0
    loud = False
    tail = []
    with open(logp, 'a', encoding='utf-8', errors='replace') as log:
        log.write(f'\n> {" ".join(cmd)}\n')
        for b in p.stdout:
            line = b.decode('utf-8', 'replace').rstrip('\r\n')
            log.write(line + '\n')
            tail = (tail + [line])[-40:]
            m = re.match(r'\[(\d+)/(\d+)\]', line)
            if m:
                loud = False
                n, total = int(m.group(1)), int(m.group(2))
                with lock:
                    state['line'] = f'  {what} {n}/{total} ({100 * n // max(total, 1)}%)'
                    show()
                continue
            if line.startswith(('FAILED:', 'ninja: error', 'CMake Error')):
                loud = True
            if loud or re.search(r'(?:^|[\s:)])(?:fatal )?error(?: [A-Z]+\d+)?:', line):
                errors += 1
                above('  ' + line)
    rc = p.wait()
    done.set()
    th.join()
    with lock:
        show(final=True)
    if rc and not errors:
        for l in tail[-25:]:
            say('  ' + l)
    if rc:
        say(f'  the full build output is in {logp}')
    return rc


class Toolchain:
    def __init__(self, llvm_bin, cmake, ninja, env, xwin_msc=None):
        self.llvm_bin, self.cmake, self.ninja, self.env, self.xwin_msc = llvm_bin, cmake, ninja, env, xwin_msc

    def configure_args(self):
        b = fwd(self.llvm_bin)
        args = ['-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={fwd(self.ninja)}']
        if self.xwin_msc:
            args += [f'-DCMAKE_TOOLCHAIN_FILE={fwd(os.path.join(ROOT, "setup", "clang-cl-xwin.cmake"))}',
                     f'-DGL_LLVM_BIN={b}', f'-DGL_MSVC_COMPAT={self.xwin_msc}',
                     '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>']
        else:
            args += [f'-DCMAKE_C_COMPILER={b}/clang-cl.exe', f'-DCMAKE_CXX_COMPILER={b}/clang-cl.exe',
                     f'-DCMAKE_LINKER={b}/lld-link.exe', f'-DCMAKE_RC_COMPILER={b}/llvm-rc.exe']
        return args

    def cmake_build(self, src, bdir, targets, build_type='Release'):
        cache = os.path.join(bdir, 'CMakeCache.txt')
        mode = 'xwin' if self.xwin_msc else 'vs'
        if os.path.isfile(cache) and f'GL_TOOLCHAIN:STRING={mode}' not in open(cache, errors='replace').read():
            say(f'  {bdir} was configured with another compiler setup: configuring it again')
            for f in ('CMakeCache.txt', 'build.ninja'):
                if os.path.isfile(os.path.join(bdir, f)):
                    os.remove(os.path.join(bdir, f))
            shutil.rmtree(os.path.join(bdir, 'CMakeFiles'), ignore_errors=True)
        if not os.path.isfile(os.path.join(bdir, 'build.ninja')):
            cfg = [self.cmake, '-S', fwd(src), '-B', fwd(bdir), *self.configure_args(),
                   f'-DCMAKE_BUILD_TYPE={build_type}', f'-DGL_TOOLCHAIN:STRING={mode}',
                   f'-DPython3_EXECUTABLE={fwd(sys.executable)}']
            if run_quiet(cfg, env=self.env, what='configuring'):
                return 5
        return run_quiet([self.ninja, '-C', bdir, *targets], env=self.env)


def find():
    """The toolchain to build with, or None (after saying what is missing)."""
    if os.name != 'nt':
        say('  The PC build needs Windows.')
        return None
    cmake, ninja = pip_tool('cmake'), pip_tool('ninja')
    if not cmake or not ninja:
        say('  CMake / Ninja missing: run SETUP.bat (it installs them into .venv).')
        return None
    vs_path, vcvars = find_vs()
    llvm = find_llvm(vs_path)
    if not llvm:
        say(f'  No LLVM {MIN_CLANG} or newer is installed: using a portable one (nothing is installed).')
        if not download_llvm():
            say('  Run the setup again to retry the download, or install LLVM (winget install LLVM.LLVM).')
            return None
        llvm = find_llvm(vs_path)
        if not llvm:
            say('  the portable LLVM does not work on this PC.')
            return None
    ver, bindir, text = llvm
    env = dict(os.environ)
    if vcvars:
        vs_env = vcvars_env(vcvars)
        if vs_env:
            say(f'  compiler: {text} ({bindir}); Windows SDK from Visual Studio ({vs_path})')
            return Toolchain(bindir, cmake, ninja, vs_env)
        say('  Visual Studio\'s vcvars64.bat did not work: using the portable SDK instead.')
    sdk = xwin_sdk()
    if not sdk:
        return None
    inc, lib, msc = xwin_paths(sdk)
    env.update(INCLUDE=';'.join(inc), LIB=';'.join(lib))
    env.pop('EXTERNAL_INCLUDE', None)
    say(f'  compiler: {text} ({bindir}); Microsoft CRT/SDK from xwin ({sdk})')
    return Toolchain(bindir, cmake, ninja, env, xwin_msc=msc)
