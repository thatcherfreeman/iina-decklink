cmake_minimum_required(VERSION 3.20)
project(iina_decklink_helper CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

if(NOT APPLE)
    message(FATAL_ERROR "The IINA DeckLink helper is macOS-only (IINA is a macOS app).")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(SDK_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../DeckLinkSDK/Mac/include")
if(NOT EXISTS "${SDK_INCLUDE}/DeckLinkAPI.h")
    message(FATAL_ERROR
        "DeckLink SDK headers not found at ${SDK_INCLUDE}. Download the SDK from "
        "blackmagicdesign.com/support and place it at DeckLinkSDK/ in the repo root.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavfilter libavutil libswscale libswresample)

add_executable(iina-decklink-helper
    main.cpp
    log.cpp
    decoder.cpp
    converter.cpp
    output.cpp
    player.cpp
    stills.cpp
    enumerate.cpp
    json.cpp
    ws_client.cpp
    serve.cpp
    decklink_shim.cpp
    # Blackmagic's dispatch shim dlopens DeckLinkAPI.framework at runtime, so
    # the helper has no link-time dependency on any proprietary binary and
    # still runs (reporting zero devices) with no driver installed.
    "${SDK_INCLUDE}/DeckLinkAPIDispatch.cpp")

target_include_directories(iina-decklink-helper PRIVATE "${SDK_INCLUDE}")

target_link_libraries(iina-decklink-helper PRIVATE
    PkgConfig::FFMPEG
    "-framework CoreFoundation"
    "-framework CoreVideo"
    "-framework CoreMedia"
    "-framework VideoToolbox")

# Ship the binary where the plugin expects it, so a dev symlink install picks
# it up with no extra step.  scripts/package.sh rewrites the FFmpeg install
# names to @loader_path for release builds.
set(HELPER_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../plugin/bin")
set_target_properties(iina-decklink-helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${HELPER_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${HELPER_OUTPUT_DIR}")
