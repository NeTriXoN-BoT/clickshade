#!/usr/bin/env python3
"""Build ClickShade for Windows x64 using Zig's bundled C++ toolchain.

Set CLICK_SHADE_ZIG to the Zig executable, put zig on PATH, or install the
ziglang Python package. This script does not download or install software.

Examples:
  python tools/build_zig.py
  python tools/build_zig.py --test
  python tools/build_zig.py --debug --output-dir build/debug

The generated binaries run on Windows. --test also builds the native integration
checks; run them in an interactive test desktop with ClickShade closed.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
LIBRARIES = (
    "user32", "gdi32", "comctl32", "shell32", "dwmapi", "advapi32",
    "wtsapi32", "uxtheme", "ole32", "oleaut32", "secur32", "winmm", "mfuuid", "comdlg32",
)


def find_zig(override: str | None) -> Path:
    value = override or os.environ.get("CLICK_SHADE_ZIG")
    if value:
        candidate = Path(value).expanduser()
        if candidate.is_dir():
            candidate /= "zig.exe" if os.name == "nt" else "zig"
        if candidate.is_file():
            return candidate.resolve()
        resolved = shutil.which(value)
        if resolved:
            return Path(resolved)
        raise RuntimeError(f"Zig executable not found: {value}")
    resolved = shutil.which("zig")
    if resolved:
        return Path(resolved)
    package = importlib.util.find_spec("ziglang")
    if package and package.origin:
        candidate = Path(package.origin).parent / (
            "zig.exe" if os.name == "nt" else "zig"
        )
        if candidate.is_file():
            return candidate
    raise RuntimeError(
        "Zig was not found. Install Zig 0.14.1 or later, then put it on PATH "
        "or set CLICK_SHADE_ZIG to its executable."
    )


def run(command: list[str], *, cwd: Path, env: dict[str, str]) -> None:
    print("+ " + shlex.join(command), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", help="Path to the Zig executable")
    parser.add_argument("--debug", action="store_true", help="Keep debug symbols")
    parser.add_argument(
        "--test", action="store_true", help="Also build the native smoke checks"
    )
    parser.add_argument("--output-dir", default="build", help="Relative to project root")
    args = parser.parse_args()

    try:
        zig = find_zig(args.zig)
        output = (ROOT / args.output_dir).resolve()
        output.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ)
        env.setdefault("ZIG_GLOBAL_CACHE_DIR", str(output / "zig-global-cache"))
        env.setdefault("ZIG_LOCAL_CACHE_DIR", str(output / "zig-local-cache"))
        print("Compiler: " + str(zig), flush=True)
        run([str(zig), "version"], cwd=ROOT, env=env)

        resources = ROOT / "resources" / "app.rc"
        if not resources.is_file():
            candidates = list((ROOT / "resources").glob("*.rc"))
            if len(candidates) != 1:
                raise RuntimeError("Expected resources/app.rc or exactly one resources/*.rc")
            resources = candidates[0]
        compiled_resources = output / "app.res"
        run(
            [str(zig), "rc", "/c", "65001", "/fo", str(compiled_resources), resources.name],
            cwd=resources.parent,
            env=env,
        )

        common = [
            str(zig), "c++", "-target", "x86_64-windows-gnu", "-std=c++17",
            "-DUNICODE", "-D_UNICODE", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN",
            "-DWINVER=0x0a00", "-D_WIN32_WINNT=0x0a00", "-DOEMRESOURCE",
            "-Wall", "-Wextra", "-Wpedantic", "-Wno-unused-parameter",
            "-I", str(ROOT / "src"),
        ]
        common += ["-O0", "-g"] if args.debug else ["-O2", "-s", "-DNDEBUG"]
        sources = sorted((ROOT / "src").glob("*.cpp"))
        if not sources or not (ROOT / "src" / "app.cpp").is_file():
            raise RuntimeError("Application sources are incomplete: src/app.cpp is required")
        executable = output / "ClickShade.exe"
        run(
            common + ["-municode", "-Wl,--subsystem,windows"]
            + [str(source) for source in sources]
            + [str(compiled_resources), "-o", str(executable)]
            + ["-l" + library for library in LIBRARIES],
            cwd=ROOT,
            env=env,
        )
        shutil.copytree(ROOT / "sounds", output / "sounds", dirs_exist_ok=True)
        print(f"Built {executable} ({executable.stat().st_size:,} bytes)", flush=True)

        if args.test:
            shake_test = output / "shake_locator_test.exe"
            run(common + ["-Wl,--subsystem,console", str(ROOT / "tests" / "shake_locator_test.cpp"),
                          "-o", str(shake_test)], cwd=ROOT, env=env)
            print(f"Built {shake_test} (headless shake gesture/animation checks).", flush=True)
            pacing_test = output / "frame_pacing_test.exe"
            run(common + ["-Wl,--subsystem,console", str(ROOT / "tests" / "frame_pacing_test.cpp"),
                          "-o", str(pacing_test)], cwd=ROOT, env=env)
            print(f"Built {pacing_test} (headless adaptive frame-budget checks).", flush=True)
            geometry_test = output / "ui_scale_test.exe"
            run(common + ["-Wl,--subsystem,console", str(ROOT / "tests" / "ui_scale_test.cpp"),
                          "-o", str(geometry_test)], cwd=ROOT, env=env)
            print(f"Built {geometry_test} (headless DPI/layout geometry checks).", flush=True)
            hotkeys_test = output / "hotkeys_test.exe"
            run(common + ["-municode", "-Wl,--subsystem,console",
                          str(ROOT / "tests" / "hotkeys_test.cpp"), "-o", str(hotkeys_test)],
                cwd=ROOT, env=env)
            print(f"Built {hotkeys_test} (headless shortcut parsing checks).", flush=True)
            key_display_test = output / "key_display_test.exe"
            run(common + ["-municode", "-Wl,--subsystem,console",
                          str(ROOT / "tests" / "key_display_test.cpp"), "-o", str(key_display_test), "-luser32"],
                cwd=ROOT, env=env)
            print(f"Built {key_display_test} (headless key-display label and policy checks).", flush=True)
            test_source = ROOT / "tests" / "cursor_smoke.cpp"
            engine_source = ROOT / "src" / "cursor_engine.cpp"
            if not test_source.is_file() or not engine_source.is_file():
                raise RuntimeError("Cursor smoke test sources are missing")
            test_executable = output / "cursor_smoke.exe"
            run(
                common + ["-municode", "-Wl,--subsystem,console", str(test_source),
                          str(engine_source), "-o", str(test_executable),
                          "-luser32", "-lgdi32", "-ladvapi32"],
                cwd=ROOT,
                env=env,
            )
            print(f"Built {test_executable}", flush=True)
            print("Run cursor_smoke.exe --run in an interactive Windows test desktop.", flush=True)
            audio_test = output / "audio_smoke.exe"
            run(
                common + ["-municode", "-Wl,--subsystem,console",
                          str(ROOT / "tests" / "audio_smoke.cpp"), "-o", str(audio_test),
                          "-luser32", "-lwinmm", "-lmfuuid", "-lole32"],
                cwd=ROOT,
                env=env,
            )
            print(f"Built {audio_test} (decoder/worker checks; audible playback is not asserted).", flush=True)
            streaming_test = output / "audio_streaming_test.exe"
            run(common + ["-municode", "-Wl,--subsystem,console",
                          str(ROOT / "tests" / "audio_streaming_test.cpp"), "-o", str(streaming_test),
                          "-luser32", "-lwinmm", "-lmfuuid", "-lole32"], cwd=ROOT, env=env)
            print(f"Built {streaming_test} (bounded streaming worker checks with an isolated fake driver).", flush=True)
            profile_test = output / "settings_profile_test.exe"
            run(common + ["-municode", "-Wl,--subsystem,console",
                          str(ROOT / "tests" / "settings_profile_test.cpp"), str(ROOT / "src" / "settings.cpp"),
                          "-o", str(profile_test), "-luser32", "-lshell32", "-ladvapi32", "-lole32", "-loleaut32", "-lsecur32"], cwd=ROOT, env=env)
            print(f"Built {profile_test} (headless profile validation/persistence checks).", flush=True)
            runtime_test = output / "runtime_smoke.exe"
            run(
                common + ["-municode", "-Wl,--subsystem,console",
                          str(ROOT / "tests" / "runtime_smoke.cpp"),
                          str(ROOT / "src" / "runtime.cpp"),
                          str(ROOT / "src" / "cursor_engine.cpp"),
                          str(ROOT / "src" / "click_audio.cpp"),
                          str(ROOT / "src" / "visual_effects.cpp"), "-o", str(runtime_test),
                          "-luser32", "-lgdi32", "-ladvapi32", "-ldwmapi", "-lwtsapi32",
                          "-lwinmm", "-lmfuuid", "-lole32", "-loleaut32"],
                cwd=ROOT,
                env=env,
            )
            print(f"Built {runtime_test}; run it with --run in an interactive test desktop.", flush=True)
            visual_test = output / "visual_smoke.exe"
            run(
                common + ["-municode", "-Wl,--subsystem,console",
                          str(ROOT / "tests" / "visual_smoke.cpp"),
                          str(ROOT / "src" / "visual_effects.cpp"), "-o", str(visual_test),
                          "-luser32", "-lgdi32"],
                cwd=ROOT,
                env=env,
            )
            print(f"Built {visual_test}; run it with --run in an interactive test desktop.", flush=True)
        return 0
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        print(f"Build failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
