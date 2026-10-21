#!/usr/bin/env python3
"""Package built Windows/Linux binaries plus the complete source.

No builds, downloads, installs, or publication. ZIP CRCs, architecture, freshness,
executable bits, source/binary identity and every SHA-256 entry are verified.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
from pathlib import Path
import stat
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = '1.0'
SOURCE_DIRS = ('src', 'resources', 'tests', 'tools', 'sounds', 'THIRD_PARTY_NOTICES',
               'linux', 'assets', 'docs', '.github')
SOURCE_ROOT_FILES = ('CMakeLists.txt', 'README.md', 'LICENSE', 'CHANGELOG.md',
    'CONTRIBUTING.md', 'SECURITY.md', 'RELEASE-NOTES.md', 'RELEASE-README.md',
    'build-windows.cmd', 'Restore-cursors.cmd', '.gitignore', '.gitattributes')
BUILD_DEBRIS = {'.pyc', '.o', '.obj', '.exe', '.pdb', '.ilk', '.log', '.tmp', '.bak', '.bmp', '.ini'}


def windows_x64(data: bytes, subsystem: int = 2) -> bool:
    if len(data) < 256 or data[:2] != b'MZ':
        return False
    offset = struct.unpack_from('<I', data, 0x3c)[0]
    if offset + 96 > len(data) or data[offset:offset+4] != b'PE\0\0':
        return False
    return (struct.unpack_from('<H', data, offset+4)[0] == 0x8664 and
            struct.unpack_from('<H', data, offset+24)[0] == 0x20b and
            struct.unpack_from('<H', data, offset+24+68)[0] == subsystem)


def linux_x64(data: bytes) -> bool:
    return (len(data) >= 64 and data[:6] == b'\x7fELF\x02\x01' and
            struct.unpack_from('<H', data, 18)[0] == 62 and
            struct.unpack_from('<H', data, 16)[0] in (2, 3))


def newest_code(*folders: str) -> float:
    return max(p.stat().st_mtime for folder in folders for p in (ROOT/folder).rglob('*')
               if p.is_file() and p.suffix in ('.cpp', '.h', '.rc', '.ico', '.manifest'))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', default='build', help='Directory with Windows application EXE')
    parser.add_argument('--linux-build-dir', default='build-linux')
    parser.add_argument('--output', help='Output ZIP (default: ../ClickShade-<version>-Windows-Linux.zip)')
    parser.add_argument('--compiler', default='Windows: Zig 0.14.1, C++17 -O2; Linux: GCC 13.3, C++17 -O2 (Ubuntu 24.04)')
    parser.add_argument('--expected-revision', help='CI: verify both BUILD-REVISION.txt files and the source Git HEAD instead of archive mtimes')
    args = parser.parse_args()
    build = (ROOT/args.build_dir).resolve()
    linux_build = (ROOT/args.linux_build_dir).resolve()
    default_name = f'../ClickShade-{VERSION}-Windows-Linux.zip'
    output = (ROOT/(args.output or default_name)).resolve()
    windows_path, linux_path = build/'ClickShade.exe', linux_build/'ClickShade'
    windows, linux = windows_path.read_bytes(), linux_path.read_bytes()
    if not windows_x64(windows):
        raise SystemExit('Expected an AMD64 PE32+ Windows GUI executable')
    if not linux_x64(linux):
        raise SystemExit('Expected an x86-64 little-endian Linux ELF executable')
    if args.expected_revision:
        revision = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
        if revision != args.expected_revision or len(revision) != 40:
            raise SystemExit('Source checkout differs from expected revision')
        for folder in (build, linux_build):
            if (folder/'BUILD-REVISION.txt').read_text().strip() != revision:
                raise SystemExit(f'Compiled artifact revision differs from source: {folder}')
        subprocess.run(['git', '-C', str(ROOT), 'diff', '--exit-code', 'HEAD', '--',
                        'src', 'resources', 'linux', 'tests', 'tools', 'CMakeLists.txt'], check=True)
    else:
        if windows_path.stat().st_mtime < newest_code('src', 'resources'):
            raise SystemExit('Windows sources/resources are newer than ClickShade.exe; rebuild first')
        shared_headers = (ROOT/'src/frame_pacing.h', ROOT/'src/shake_locator.h')
        linux_newest = max(newest_code('linux'), *(path.stat().st_mtime for path in shared_headers))
        if linux_path.stat().st_mtime < linux_newest:
            raise SystemExit('Linux or shared sources are newer than ClickShade; rebuild first')

    entries: dict[str, tuple[bytes, int]] = {}
    def add(name: str, data: bytes, executable: bool = False) -> None:
        if name in entries:
            raise RuntimeError(f'Duplicate archive path: {name}')
        entries[name] = (data, 0o755 if executable else 0o644)
    def add_file(name: str, path: Path, executable: bool | None = None) -> None:
        mode = bool(path.stat().st_mode & stat.S_IXUSR) if executable is None else executable
        add(name, path.read_bytes(), mode)

    add('Windows/ClickShade.exe', windows)
    add('Linux/ClickShade', linux, True)
    add_file('Windows/Restore-cursors.cmd', ROOT/'Restore-cursors.cmd')
    add_file('Linux/README.md', ROOT/'linux/README.md')
    add_file('Linux/clickshade.svg', ROOT/'assets/clickshade.svg')
    add_file('Linux/clickshade.desktop', ROOT/'linux/clickshade.desktop')
    add_file('LICENSE', ROOT/'LICENSE')
    add_file('README.md', ROOT/'RELEASE-README.md')
    add_file('RELEASE-NOTES.md', ROOT/'RELEASE-NOTES.md')
    for name in SOURCE_ROOT_FILES:
        add_file('source/'+name, ROOT/name)
    for folder in SOURCE_DIRS:
        for path in sorted((ROOT/folder).rglob('*')):
            if (not path.is_file() or any(part in ('__pycache__', 'node_modules', '.git', '.cache', 'Testing')
                                        for part in path.parts) or path.suffix in BUILD_DEBRIS or
                    path.name == 'settings.ini'):
                continue
            if path.is_symlink():
                raise SystemExit(f'Refusing to package a source symlink: {path}')
            relative = path.relative_to(ROOT).as_posix()
            add_file('source/'+relative, path)
            if folder == 'sounds':
                add_file('Windows/'+relative, path)
                add_file('Linux/'+relative, path)
            elif folder == 'THIRD_PARTY_NOTICES':
                add_file(relative, path)
            elif relative.startswith('linux/gnome-extension/') and path.name in (
                    'README.md', 'install.sh', 'metadata.json', 'core.js', 'extension.js'):
                add_file('Linux/gnome-extension/'+path.relative_to(ROOT/'linux/gnome-extension').as_posix(), path)
    add('BUILD-INFO.txt', (
        f'ClickShade {VERSION}\nPackaged UTC: {datetime.now(timezone.utc).isoformat(timespec="seconds")}\n'
        f'{args.compiler}\n'
        f'Source revision: {args.expected_revision or "local source freshness verified"}\n'
        'Windows target: Windows 10 (version 1703 or later) / Windows 11 x64, PE32+ AMD64 GUI subsystem; unsigned.\n'
        'Linux target: x86-64 ELF, system GTK3/X11/Cairo runtime; supplied local build requires GLIBC_2.38 and GLIBCXX_3.4.32.\n'
        f'Windows/ClickShade.exe: {len(windows):,} bytes; SHA256 {hashlib.sha256(windows).hexdigest()}\n'
        f'Linux/ClickShade: {len(linux):,} bytes; SHA256 {hashlib.sha256(linux).hexdigest()}\n'
        'Build recipes: tools/build_zig.py --test; tools/build_linux.py --test, or documented CMake paths.\n'
        'Source is complete and matching; compiler SDKs/runtime libraries are not included.\n'
        'Normal app folders contain no compiled test tools, debug symbols or logs.\n'
        'CHECKSUMS.sha256 covers every other file in this ZIP.\n').encode())
    add('CHECKSUMS.sha256', ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n'
        for name, (data, _) in sorted(entries.items())).encode())
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, (data, mode) in sorted(entries.items()):
            item = zipfile.ZipInfo(name)
            item.create_system = 3
            item.external_attr = (stat.S_IFREG | mode) << 16
            item.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(item, data, compresslevel=9)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise SystemExit('ZIP CRC validation failed')
        for line in archive.read('CHECKSUMS.sha256').decode().splitlines():
            expected, name = line.split('  ', 1)
            if hashlib.sha256(archive.read(name)).hexdigest() != expected:
                raise SystemExit(f'Checksum mismatch: {name}')
        for name, data in (('Windows/ClickShade.exe', windows), ('Linux/ClickShade', linux)):
            if archive.read(name) != data:
                raise SystemExit(f'Packaged binary mismatch: {name}')
        if not (archive.getinfo('Linux/ClickShade').external_attr >> 16) & stat.S_IXUSR:
            raise SystemExit('Linux executable permission was lost')
    print(f'Created {output} ({output.stat().st_size:,} bytes, {len(entries)} files)')
    print('Verified architectures, source freshness, ZIP CRCs, SHA-256, binary identity and executable permissions.')

if __name__ == '__main__':
    main()
