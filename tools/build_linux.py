#!/usr/bin/env python3
"""Build the native Linux binary with system deps or an extracted developer SDK.

No downloads or installs. Normal users can run linux/build.sh instead.
The isolated SDK option is for reproducible build/CI environments only; SDK files
are never bundled into the release and no runtime rpath is embedded.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=os.environ.get("CLICK_SHADE_LINUX_SDK"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--debug", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = (args.output or root / "build-linux" / "ClickShade").resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    if args.sdk:
        sdk = Path(args.sdk).resolve()
        lib = sdk / "usr/lib/x86_64-linux-gnu"
        env["PKG_CONFIG_SYSROOT_DIR"] = str(sdk)
        env["PKG_CONFIG_PATH"] = ":".join(map(str, (lib / "pkgconfig", sdk / "usr/share/pkgconfig", Path("/usr/lib/x86_64-linux-gnu/pkgconfig"))))
        env["LD_LIBRARY_PATH"] = str(lib) + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
        pkg = str(sdk / "usr/bin/pkgconf")
    else:
        pkg = env.get("PKG_CONFIG") or shutil.which("pkg-config") or shutil.which("pkgconf")
        if not pkg:
            parser.error("pkg-config is missing; see linux/README.md for development dependencies")
    modules = ["gtk+-3.0", "gio-2.0", "x11", "xi", "xfixes", "xrender", "xext", "xrandr", "cairo"]
    cflags = shlex.split(subprocess.check_output([pkg, "--cflags", *modules, "libpulse"], env=env, text=True))
    libs = shlex.split(subprocess.check_output([pkg, "--libs", *modules], env=env, text=True))
    cxx = shlex.split(env.get("CXX", "g++"))
    sources = ["app.cpp", "settings.cpp", "hotkeys.cpp", "x11_engine.cpp", "engine_factory.cpp", "gnome_adapter.cpp", "audio.cpp"]
    command = [*cxx, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Wpedantic", "-Wno-deprecated-declarations",
               *( ["-O0", "-g"] if args.debug else ["-O2", "-DNDEBUG", "-s"] ),
               *shlex.split(env.get("CXXFLAGS", "")), *cflags,
               *(str(root / "linux" / name) for name in sources),
               "-o", str(output), *libs, "-ldl", *shlex.split(env.get("LDFLAGS", ""))]
    print("Building native Linux x64:", output, flush=True)
    subprocess.run(command, check=True, env=env)
    shutil.copytree(root / "sounds", output.parent / "sounds", dirs_exist_ok=True)
    print(f"Built {output.stat().st_size:,} bytes", flush=True)
    if args.test:
        # Developer tests are separate binaries, never linked into the GUI.
        shake_test = output.parent / "shake_locator_test"
        subprocess.run([*cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic",
                        *shlex.split(env.get("CXXFLAGS", "")), "-I", str(root / "src"),
                        str(root / "tests/shake_locator_test.cpp"), "-o", str(shake_test),
                        *shlex.split(env.get("LDFLAGS", ""))], env=env, check=True)
        subprocess.run([str(shake_test)], check=True)
        pacing_test = output.parent / "frame_pacing_test"
        subprocess.run([*cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic",
                        *shlex.split(env.get("CXXFLAGS", "")), "-I", str(root / "src"),
                        str(root / "tests/frame_pacing_test.cpp"), "-o", str(pacing_test),
                        *shlex.split(env.get("LDFLAGS", ""))], env=env, check=True)
        subprocess.run([str(pacing_test)], check=True)
        settings_test = output.parent / "settings_test"
        glib_cflags = shlex.split(subprocess.check_output([pkg, "--cflags", "glib-2.0"], env=env, text=True))
        glib_libs = shlex.split(subprocess.check_output([pkg, "--libs", "glib-2.0"], env=env, text=True))
        subprocess.run([*cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic",
                        *shlex.split(env.get("CXXFLAGS", "")), *glib_cflags,
                        str(root / "linux/self_test.cpp"), str(root / "linux/settings.cpp"),
                        "-o", str(settings_test), *glib_libs, *shlex.split(env.get("LDFLAGS", ""))],
                       env=env, check=True)
        # Run against ordinary system runtime libraries, not the extracted SDK.
        subprocess.run([str(settings_test)], check=True)
        audio_test = output.parent / "audio_smoke"
        pulse_cflags = shlex.split(subprocess.check_output([pkg, "--cflags", "libpulse"], env=env, text=True))
        subprocess.run([*cxx, "-std=c++17", "-O2", "-pthread", "-Wall", "-Wextra", "-Wpedantic",
                        *shlex.split(env.get("CXXFLAGS", "")), *pulse_cflags,
                        str(root / "linux/tests/audio_smoke.cpp"), "-o", str(audio_test), "-ldl",
                        *shlex.split(env.get("LDFLAGS", ""))], env=env, check=True)
        subprocess.run([str(audio_test)], check=True)
        queue_test = output.parent / "gnome_queue_smoke"
        gio_cflags = shlex.split(subprocess.check_output([pkg, "--cflags", "gio-2.0", "libpulse"], env=env, text=True))
        gio_libs = shlex.split(subprocess.check_output([pkg, "--libs", "gio-2.0"], env=env, text=True))
        subprocess.run([*cxx, "-std=c++17", "-O2", "-pthread", "-Wall", "-Wextra", "-Wpedantic",
                        *shlex.split(env.get("CXXFLAGS", "")), *gio_cflags, "-I", str(root / "linux"),
                        str(root / "linux/tests/gnome_queue_smoke.cpp"), str(root / "linux/gnome_adapter.cpp"),
                        str(root / "linux/audio.cpp"), "-o", str(queue_test), *gio_libs, "-ldl",
                        *shlex.split(env.get("LDFLAGS", ""))], env=env, check=True)
        # Uses a private temporary D-Bus daemon; never contacts the user's Shell.
        subprocess.run([str(queue_test)], check=True, timeout=20)
        xtst = shlex.split(subprocess.check_output([pkg, "--libs", "xtst"], env=env, text=True))
        hotkeys_test = output.parent / "hotkeys_smoke"
        subprocess.run([*cxx, "-std=c++17", "-O2", "-pthread", "-Wall", "-Wextra", "-Wpedantic",
                        *cflags, "-I", str(root / "linux"), str(root / "linux/tests/hotkeys_smoke.cpp"),
                        str(root / "linux/hotkeys.cpp"), "-o", str(hotkeys_test), *libs, *xtst],
                       env=env, check=True)
        subprocess.run([str(hotkeys_test), "--headless"], env=env, check=True)
        interactive = output.parent / "x11_smoke"
        test_command = [*cxx, "-std=c++17", "-O2", "-pthread", "-Wall", "-Wextra", "-Wpedantic",
                        *cflags, "-I", str(root / "linux"), str(root / "linux/tests/x11_smoke.cpp"),
                        *(str(root / "linux" / name) for name in ["x11_engine.cpp", "audio.cpp", "settings.cpp"]),
                        "-o", str(interactive), *libs, *xtst, "-ldl"]
        subprocess.run(test_command, env=env, check=True)
        print(f"Built {interactive}; interactive input test is not run automatically (requires --run).", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
