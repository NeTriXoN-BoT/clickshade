find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(CLICKSHADE_LINUX REQUIRED IMPORTED_TARGET
    gtk+-3.0 gio-2.0 x11 xi xfixes xrender xext xrandr cairo)
# PulseAudio is optional at runtime (dlopen), but its headers are required to
# compile the bounded audio worker. Do not link the client library here.
pkg_check_modules(CLICKSHADE_PULSE_HEADERS REQUIRED libpulse)

add_executable(ClickShadeLinux
    app.cpp settings.cpp hotkeys.cpp x11_engine.cpp engine_factory.cpp
    gnome_adapter.cpp audio.cpp)
set_target_properties(ClickShadeLinux PROPERTIES
    OUTPUT_NAME ClickShade
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_custom_command(TARGET ClickShadeLinux POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../sounds" "$<TARGET_FILE_DIR:ClickShadeLinux>/sounds"
    VERBATIM)
target_compile_features(ClickShadeLinux PRIVATE cxx_std_17)
target_include_directories(ClickShadeLinux PRIVATE ${CLICKSHADE_PULSE_HEADERS_INCLUDE_DIRS})
target_compile_options(ClickShadeLinux PRIVATE -Wall -Wextra -Wpedantic -Wno-deprecated-declarations)
target_link_libraries(ClickShadeLinux PRIVATE PkgConfig::CLICKSHADE_LINUX Threads::Threads ${CMAKE_DL_LIBS})

if(CLICKSHADE_BUILD_TESTS)
    enable_testing()
    add_executable(ClickShadeLinuxSettingsTest self_test.cpp settings.cpp)
    set_target_properties(ClickShadeLinuxSettingsTest PROPERTIES
        OUTPUT_NAME settings_test
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_features(ClickShadeLinuxSettingsTest PRIVATE cxx_std_17)
    target_compile_options(ClickShadeLinuxSettingsTest PRIVATE -Wall -Wextra -Wpedantic)
    pkg_check_modules(CLICKSHADE_GLIB REQUIRED IMPORTED_TARGET glib-2.0)
    target_link_libraries(ClickShadeLinuxSettingsTest PRIVATE PkgConfig::CLICKSHADE_GLIB)
    add_test(NAME LinuxSettings COMMAND ClickShadeLinuxSettingsTest)
    # Includes audio.cpp directly to test the private decoder; do not link it twice.
    add_executable(ClickShadeLinuxAudioTest tests/audio_smoke.cpp)
    target_compile_features(ClickShadeLinuxAudioTest PRIVATE cxx_std_17)
    target_include_directories(ClickShadeLinuxAudioTest PRIVATE ${CLICKSHADE_PULSE_HEADERS_INCLUDE_DIRS})
    target_compile_options(ClickShadeLinuxAudioTest PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ClickShadeLinuxAudioTest PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME LinuxAudioFiles COMMAND ClickShadeLinuxAudioTest)
    pkg_check_modules(CLICKSHADE_GIO REQUIRED IMPORTED_TARGET gio-2.0)
    add_executable(ClickShadeGnomeQueueTest tests/gnome_queue_smoke.cpp gnome_adapter.cpp audio.cpp)
    target_compile_features(ClickShadeGnomeQueueTest PRIVATE cxx_std_17)
    target_include_directories(ClickShadeGnomeQueueTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" ${CLICKSHADE_PULSE_HEADERS_INCLUDE_DIRS})
    target_compile_options(ClickShadeGnomeQueueTest PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ClickShadeGnomeQueueTest PRIVATE PkgConfig::CLICKSHADE_GIO Threads::Threads ${CMAKE_DL_LIBS})
    add_test(NAME LinuxGnomeBridgeQueue COMMAND ClickShadeGnomeQueueTest)
    set_tests_properties(LinuxGnomeBridgeQueue PROPERTIES TIMEOUT 20)
    pkg_check_modules(CLICKSHADE_XTEST REQUIRED IMPORTED_TARGET xtst)
    add_executable(ClickShadeHotkeysTest tests/hotkeys_smoke.cpp hotkeys.cpp)
    target_include_directories(ClickShadeHotkeysTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(ClickShadeHotkeysTest PRIVATE cxx_std_17)
    target_compile_options(ClickShadeHotkeysTest PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ClickShadeHotkeysTest PRIVATE PkgConfig::CLICKSHADE_LINUX
        PkgConfig::CLICKSHADE_XTEST Threads::Threads)
    add_test(NAME LinuxHotkeyConfig COMMAND ClickShadeHotkeysTest --headless)
    add_executable(ClickShadeX11Test tests/x11_smoke.cpp x11_engine.cpp audio.cpp settings.cpp)
    target_include_directories(ClickShadeX11Test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" ${CLICKSHADE_PULSE_HEADERS_INCLUDE_DIRS})
    target_compile_features(ClickShadeX11Test PRIVATE cxx_std_17)
    target_compile_options(ClickShadeX11Test PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(ClickShadeX11Test PRIVATE PkgConfig::CLICKSHADE_LINUX
        PkgConfig::CLICKSHADE_XTEST Threads::Threads ${CMAKE_DL_LIBS})
    # Not registered as an automatic CTest: this test injects pointer input and
    # needs --run on an explicitly selected disposable X11 desktop.
endif()

install(TARGETS ClickShadeLinux RUNTIME DESTINATION bin)
install(DIRECTORY ../sounds/ DESTINATION bin/sounds)
install(FILES ../assets/clickshade.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES clickshade.desktop DESTINATION share/applications)
