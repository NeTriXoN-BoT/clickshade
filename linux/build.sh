#!/usr/bin/env sh
set -eu

# Normal native build. Install GTK3/X11 development packages with your distro's
# package manager first; this script never installs or downloads dependencies.
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_build=${CLICK_SHADE_LINUX_BUILD:-"$task_root/build-linux"}
mkdir -p "$task_build"
pkg_config=${PKG_CONFIG:-pkg-config}
cxx=${CXX:-c++}
modules="gtk+-3.0 gio-2.0 x11 xi xfixes xrender xext xrandr cairo"
if ! "$pkg_config" --exists $modules libpulse; then
    echo 'Missing GTK3/X11/PulseAudio development packages. See linux/README.md.' >&2
    exit 1
fi
# pkg-config emits shell words by contract. User CXXFLAGS/LDFLAGS are conventional
# build flags; file paths are passed separately and remain quoted.
# shellcheck disable=SC2086
"$cxx" -std=c++17 -O2 -DNDEBUG -pthread -Wall -Wextra -Wpedantic \
    -Wno-deprecated-declarations ${CXXFLAGS:-} \
    $("$pkg_config" --cflags $modules libpulse) \
    "$task_root/linux/app.cpp" "$task_root/linux/settings.cpp" \
    "$task_root/linux/hotkeys.cpp" \
    "$task_root/linux/x11_engine.cpp" \
    "$task_root/linux/engine_factory.cpp" "$task_root/linux/gnome_adapter.cpp" \
    "$task_root/linux/audio.cpp" \
    -o "$task_build/ClickShade" \
    $("$pkg_config" --libs $modules) -ldl ${LDFLAGS:-}
mkdir -p "$task_build/sounds"
cp -R "$task_root/sounds/." "$task_build/sounds/"
echo "Built $task_build/ClickShade"
if [ "${1:-}" = "--test" ]; then
    # Developer-only executable; the installed GUI contains no self-test mode.
    # shellcheck disable=SC2086
    "$cxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic ${CXXFLAGS:-} \
        -I"$task_root/src" "$task_root/tests/shake_locator_test.cpp" \
        -o "$task_build/shake_locator_test" ${LDFLAGS:-}
    "$task_build/shake_locator_test"
    # shellcheck disable=SC2086
    "$cxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic ${CXXFLAGS:-} \
        -I"$task_root/src" "$task_root/tests/frame_pacing_test.cpp" \
        -o "$task_build/frame_pacing_test" ${LDFLAGS:-}
    "$task_build/frame_pacing_test"
    # shellcheck disable=SC2086
    "$cxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic ${CXXFLAGS:-} \
        $("$pkg_config" --cflags glib-2.0) \
        "$task_root/linux/self_test.cpp" "$task_root/linux/settings.cpp" \
        -o "$task_build/settings_test" $("$pkg_config" --libs glib-2.0) ${LDFLAGS:-}
    "$task_build/settings_test"
    # Parser and nonregular-file rejection checks need no display or device.
    # shellcheck disable=SC2086
    "$cxx" -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic ${CXXFLAGS:-} \
        $("$pkg_config" --cflags libpulse) "$task_root/linux/tests/audio_smoke.cpp" \
        -o "$task_build/audio_smoke" -ldl ${LDFLAGS:-}
    "$task_build/audio_smoke"
    # Native bridge checks use a private mock D-Bus daemon, not GNOME Shell.
    # shellcheck disable=SC2086
    "$cxx" -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic ${CXXFLAGS:-} \
        $("$pkg_config" --cflags gio-2.0 libpulse) -I"$task_root/linux" \
        "$task_root/linux/tests/gnome_queue_smoke.cpp" \
        "$task_root/linux/gnome_adapter.cpp" "$task_root/linux/audio.cpp" \
        -o "$task_build/gnome_queue_smoke" $("$pkg_config" --libs gio-2.0) -ldl ${LDFLAGS:-}
    "$task_build/gnome_queue_smoke"
fi
