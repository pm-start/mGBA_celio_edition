#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Make the distribution zip of the rom128 build and check its contents.

    python tools/rom128/package.py --release 5 --dist <dist folder from build_win.sh> [--src <source>] [--out <folder>]
    python tools/rom128/package.py --verify-zip <zip>        (only check an existing zip)

Output: mGBA-celio-2.0.0-rom128-<N>-win64.zip (top folder mGBA-celio-rom128-<N>) and <zip>.sha256.
  - exe, DLLs, Qt plugins from --dist (settings, logs, backups, scripts are left out)
  - LICENSE, readme.txt, the two ABI notes, source/SOURCE.txt, source/rom128-branch.diff (full diff against upstream),
    source/BUNDLED_DLLS.txt (the MSYS2 package of every DLL, for the GPL source offer)
The commit written into SOURCE.txt is the HEAD of --src by default (override with --commit).

Optional check for names that must not appear in the package: --forbid-word WORD (repeatable) or the environment
variable ROM128_FORBIDDEN_WORDS (comma separated). Each word is searched for case-insensitively in UTF-8 and UTF-16LE.
Environment: MSYS2_ROOT_WIN (MSYS2 folder for Windows, default C:/msys64), PS_ASCII_TMP (ASCII-only temp folder
for brotli check, default the system temp).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

MSYS = os.environ.get('MSYS2_ROOT_WIN', 'C:/msys64')
BASE = '3da13060a586f2da8eb4ecbb167f642e2e4889c2'   # celio edition 2.0.0
FORBIDDEN_NAMES = {'config.ini', 'qt.ini', 'library.sqlite3', 'nointro.sqlite3'}
FORBIDDEN_DIRS = ('logs/', 'backups/', 'scripts/')
BROTLI_DLLS = ('libbrotlidec.dll', 'libbrotlicommon.dll')
REQUIRED = ('mgba-celio.exe', 'portable.ini', 'readme.txt', 'LICENSE', 'source/SOURCE.txt',
            'source/rom128-branch.diff', 'source/BUNDLED_DLLS.txt', 'platforms/qwindows.dll')
REQUIRED_FFMPEG = ('avcodec-62.dll', 'libx264-165.dll')   # only when the readme says recording is included


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def msys_run(cmd):
    env = dict(os.environ, MSYSTEM='MINGW64', CHERE_INVOKING='1', LANG='C', LC_ALL='C')
    r = subprocess.run([MSYS + '/usr/bin/bash.exe', '-lc', cmd], capture_output=True, text=True, encoding='utf-8', env=env)
    if r.returncode != 0:
        raise RuntimeError('msys: %s\n%s' % (cmd, r.stderr[-800:]))
    return r.stdout


def package_dlls(dist):
    """For every DLL in dist: the MSYS2 package (name, version) it came from, and its SHA-256."""
    names = sorted(n for n in os.listdir(dist) if n.lower().endswith('.dll'))
    out = msys_run('pacman -Qo ' + ' '.join('/mingw64/bin/' + n for n in names) + ' 2>&1 || true')
    owner = {}
    for line in out.splitlines():
        m = re.match(r'/mingw64/bin/(\S+) is owned by (\S+) (\S+)', line)
        if m:
            owner[m.group(1)] = (m.group(2), m.group(3))
    return [(n, owner.get(n, ('(not found in the MSYS2 package database)', '-')), sha256(os.path.join(dist, n))) for n in names]


def expected_brotli(pkg):
    d = tempfile.mkdtemp(dir=os.environ.get('PS_ASCII_TMP'))
    try:
        shutil.copy(pkg, os.path.join(d, 'p.pkg.tar.zst'))
        msys_run('cd %s && tar --zstd -xf p.pkg.tar.zst' % d.replace('\\', '/'))
        return {n: sha256(os.path.join(d, 'mingw64', 'bin', n)) for n in BROTLI_DLLS}
    finally:
        shutil.rmtree(d, ignore_errors=True)


def git(src, *args):
    r = subprocess.run(['git', '-C', src] + list(args), capture_output=True)
    if r.returncode != 0:
        raise RuntimeError('git %s failed: %s' % (' '.join(args), r.stderr.decode('utf-8', 'replace')[-400:]))
    return r.stdout


def source_text(release, commit, ffmpeg):
    s = (
        'mGBA celio edition 2.0.0 (commit %s) + source/rom128-branch.diff.\n'
        'Release: rom128-%s\n'
        'The same changes as commits: https://github.com/pm-start/mGBA_celio_edition/tree/rom128 (commit %s).\n'
        'mgba-celio.exe was built from that commit (MSYS2 mingw64, Release, Qt/SDL/Lua, static mGBA, LTO off, CELIO_VERSION=rom128-%s).\n'
        'Built with tools/rom128/build_win.sh (see tools/rom128/README.md).\n'
        'The bundled DLLs are unmodified MSYS2 mingw64 packages (see BUNDLED_DLLS.txt); their sources: https://repo.msys2.org/mingw/sources/\n'
    ) % (BASE, release, commit, release)
    if ffmpeg:
        s += 'Video recording is enabled (cmake option USE_FFMPEG=ON); the bundled FFmpeg/x264/x265/Xvid/vid.stab DLLs are GPL (see readme.txt).\n'
    s += ('License: MPL-2.0 (mGBA)%s.\n'
          'Upstream: https://github.com/Exormeter/mGBA_celio_edition and https://github.com/mgba-emu/mgba\n') % (
        '; GPL for the bundled FFmpeg-related DLLs' if ffmpeg else '')
    return s


def build(a):
    rel = a.release
    top = 'mGBA-celio-rom128-%s' % rel
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    stage = os.path.join(out, 'stage', top)
    shutil.rmtree(os.path.join(out, 'stage'), ignore_errors=True)
    os.makedirs(os.path.join(stage, 'source'))
    ffmpeg = any(n.lower().startswith('avcodec') for n in os.listdir(a.dist))
    for n in sorted(os.listdir(a.dist)):
        p = os.path.join(a.dist, n)
        if n.lower() in FORBIDDEN_NAMES or n.lower() == 'readme.txt' or n.lower().startswith('mgba_mcp') or n in ('scripts', 'logs', 'backups'):
            continue
        if os.path.isdir(p):
            shutil.copytree(p, os.path.join(stage, n))
        else:
            shutil.copy2(p, os.path.join(stage, n))
    shutil.copy(os.path.join(a.src, 'LICENSE'), os.path.join(stage, 'LICENSE'))
    for n in ('hgx4-abi.md', 'obj256-abi.md'):
        shutil.copy(os.path.join(a.src, 'tools', n), os.path.join(stage, n))
    shutil.copy(os.path.join(a.src, 'readme.txt'), os.path.join(stage, 'readme.txt'))
    diff = git(a.src, 'diff', BASE, '--', '.', ':(exclude)build*', ':(exclude)dist*')
    with open(os.path.join(stage, 'source', 'rom128-branch.diff'), 'wb') as f:
        f.write(diff)
    commit = a.commit or git(a.src, 'rev-parse', 'HEAD').decode().strip()
    with open(os.path.join(stage, 'source', 'SOURCE.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(source_text(rel, commit, ffmpeg))
    with open(os.path.join(stage, 'source', 'BUNDLED_DLLS.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('Bundled DLLs: the MSYS2 mingw64 package each file came from (unmodified), and its SHA-256.\n'
                'Sources of every package: https://repo.msys2.org/mingw/sources/ (and https://github.com/msys2/MINGW-packages)\n\n')
        for n, (pk, ver), h in package_dlls(a.dist):
            f.write('%-34s %-48s %-24s %s\n' % (n, pk, ver, h))
    zname = 'mGBA-celio-2.0.0-rom128-%s-win64.zip' % rel
    zpath = os.path.join(out, zname)
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        files = [os.path.join(r, n) for r, _, ns in os.walk(stage) for n in ns]
        for p in sorted(files):
            z.write(p, os.path.relpath(p, os.path.dirname(stage)).replace('\\', '/'))
    shutil.rmtree(os.path.join(out, 'stage'), ignore_errors=True)
    h = sha256(zpath)
    with open(zpath + '.sha256', 'w', encoding='utf-8', newline='\n') as f:
        f.write('%s *%s\n' % (h, zname))
    print('zip: %s\nsha256: %s\nsize: %d' % (zpath, h, os.path.getsize(zpath)))
    return zpath


def verify_zip(zpath, forbid_words=(), brotli_pkg=None):
    problems = []
    z = zipfile.ZipFile(zpath)
    names = z.namelist()
    tops = {n.split('/')[0] for n in names}
    if len(tops) != 1:
        problems.append('top folder not unique: %s' % tops)
    top = sorted(tops)[0]
    rel = re.search(r'rom128-(\d+)', top)
    for n in names:
        low = n.lower()
        inner = low.split('/', 1)[-1]
        if low.rsplit('/', 1)[-1] in FORBIDDEN_NAMES:
            problems.append('forbidden file: ' + n)
        if any(inner.startswith(d) for d in FORBIDDEN_DIRS):
            problems.append('forbidden dir: ' + n)
    for need in REQUIRED:
        if top + '/' + need not in names:
            problems.append('missing: ' + need)
    readme = z.read(top + '/readme.txt').decode('utf-8-sig')
    if rel and ('リリース: rom128-%s' % rel.group(1)) not in readme:
        problems.append('readme release line does not match rom128-%s' % rel.group(1))
    if any(n.startswith(top + '/avcodec') for n in names):
        for need in REQUIRED_FFMPEG + BROTLI_DLLS:
            if top + '/' + need not in names:
                problems.append('missing: ' + need)
        for must in ('GPL', 'repo.msys2.org/mingw/sources', 'FFmpeg', 'x264', 'x265'):
            if must not in readme:
                problems.append('readme lacks: ' + must)
    words = []
    for w in forbid_words:
        words += [w.lower().encode('utf-8'), w.lower().encode('utf-16-le')]
    for n in names:
        if n.endswith('/') or not words:
            continue
        data = z.read(n).lower()
        for w in words:
            if w in data:
                problems.append('forbidden word found in %s' % n)
    if brotli_pkg:
        for n, h in expected_brotli(brotli_pkg).items():
            got = hashlib.sha256(z.read(top + '/' + n)).hexdigest()
            if got != h:
                problems.append('%s sha256 mismatch: zip %s != package %s' % (n, got, h))
            else:
                print('brotli ok: %s %s' % (n, got))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--release', type=int)
    ap.add_argument('--dist')
    ap.add_argument('--src', default=os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')))
    ap.add_argument('--commit', help='default: HEAD of --src')
    ap.add_argument('--out', default='.')
    ap.add_argument('--forbid-word', action='append', default=[])
    ap.add_argument('--brotli-pkg')
    ap.add_argument('--verify-zip')
    a = ap.parse_args()
    forbid = list(a.forbid_word) + [w for w in os.environ.get('ROM128_FORBIDDEN_WORDS', '').split(',') if w]
    if a.verify_zip:
        zp = a.verify_zip
    else:
        for k in ('release', 'dist'):
            if getattr(a, k) is None:
                ap.error('--%s is required' % k)
        zp = build(a)
    problems = verify_zip(zp, forbid, a.brotli_pkg)
    for p in problems:
        print('NG:', p)
    print('verify: %s' % ('OK' if not problems else 'NG %d' % len(problems)))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
