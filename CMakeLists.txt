cmake_minimum_required(VERSION 3.25)
project(bowsers_image_viewer LANGUAGES CXX)

# The Windows cross toolchain is selected with -DCMAKE_TOOLCHAIN_FILE=... at
# configure time. Keep the normal native build path unchanged.
option(BIV_ENABLE_LINUX_DESKTOP_INTEGRATION "Install a Linux desktop entry and icon" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# sqlite3.c (vendored below) is C, not C++.
enable_language(C)

if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-mismatched-new-delete)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BIV_ENABLE_ARCHIVES "Enable opening images directly from .zip/.rar via libarchive" ON)
option(BIV_ENABLE_AVIF "Enable AVIF decoding via libavif" ON)
option(BIV_ENABLE_WEBP "Enable WebP decoding via libwebp" ON)
option(BIV_ENABLE_VIDEO "Enable video + animated GIF playback via FFmpeg and miniaudio" ON)
option(BIV_ENABLE_JPEG "Fast JPEG thumbnails via libjpeg-turbo DCT scaling (falls back to stb_image if not found)" ON)
option(BIV_ENABLE_PNG "Fast PNG thumbnails via libpng row streaming (falls back to stb_image if not found)" ON)
option(BIV_ENABLE_SVG "Enable SVG rasterization via lunasvg" ON)
option(BIV_WINDOWS_CONSOLE "Windows: build as a console app (opens a terminal window; handy for debugging)" OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)
find_package(Threads REQUIRED)

set(SLINT_CARGO_RELEASE_PROFILE ON CACHE BOOL "Force Slint Cargo release profile" FORCE)

# Slint C++ API. Pin an exact tag so pixel-rendering behavior (nearest vs
# smooth) stays consistent across builds; bump deliberately.
FetchContent_Declare(
    Slint
    GIT_REPOSITORY https://github.com/slint-ui/slint.git
    GIT_TAG        v1.17.1
    SOURCE_SUBDIR  api/cpp
)

set(SLINT_FEATURE_INTERPRETER OFF CACHE BOOL "" FORCE)
set(SLINT_FEATURE_LIVE_PREVIEW OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(Slint)

# stb_image: vendored single header, fetched at configure time.
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(stb)

if(BIV_ENABLE_SVG)
    set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        lunasvg
        GIT_REPOSITORY https://github.com/sammycage/lunasvg.git
        GIT_TAG        v3.2.1
    )
    FetchContent_MakeAvailable(lunasvg)
endif()

if(BIV_ENABLE_ARCHIVES)
    find_package(LibArchive REQUIRED)
endif()

if(BIV_ENABLE_AVIF OR BIV_ENABLE_WEBP)
    find_package(PkgConfig REQUIRED)
endif()

if(BIV_ENABLE_AVIF)
    pkg_check_modules(AVIF REQUIRED libavif)
endif()

if(BIV_ENABLE_WEBP)
    # libwebpdemux provides WebPAnimDecoder, used to pull a still first frame
    # out of animated WebP files.
    pkg_check_modules(WEBP REQUIRED libwebp libwebpdemux)
endif()

pkg_check_modules(LIBEXIF REQUIRED libexif)

# libjpeg-turbo decodes JPEGs at 1/2, 1/4 or 1/8 size directly, which is what
# makes thumbnails of large photos fast. Optional: without it thumbnails fall
# back to a full stb_image decode.
if(BIV_ENABLE_JPEG)
    find_package(JPEG)
    if(NOT JPEG_FOUND)
        message(WARNING "libjpeg-turbo not found: JPEG thumbnails will use the slower full-size decoder")
        set(BIV_ENABLE_JPEG OFF)
    endif()
endif()

# libpng hands over PNGs one row at a time, so a thumbnail is built without ever
# holding the full-size image, and it inflates through the system zlib (often
# zlib-ng, which is much faster than stb_image's own inflate). Optional: without
# it PNG thumbnails fall back to a full stb_image decode.
if(BIV_ENABLE_PNG)
    find_package(PNG)
    if(NOT PNG_FOUND)
        message(WARNING "libpng not found: PNG thumbnails will use the slower full-size decoder")
        set(BIV_ENABLE_PNG OFF)
    endif()
endif()

if(BIV_ENABLE_VIDEO)
    find_package(PkgConfig REQUIRED)
    # FFmpeg >= 6.0 (uses the AVChannelLayout API): libavformat demuxes mkv/mp4/
    # webm/gif/..., libavcodec decodes, libswscale converts frames to RGBA,
    # libswresample converts audio for the output device.
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat>=60 libavcodec>=60 libavutil libswscale libswresample)

    # miniaudio: single public-domain header used for audio output (playback only).
    FetchContent_Declare(
        miniaudio
        URL https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_MakeAvailable(miniaudio)
endif()

# ---------------------------------------------------------------------------
# clip: cross-platform clipboard (vendored)
# ---------------------------------------------------------------------------
set(CLIP_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLIP_TESTS OFF CACHE BOOL "" FORCE)
set(CLIP_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/clip)

# ---------------------------------------------------------------------------
# sqlite3: public-domain amalgamation (vendored), backs the settings store
# (src/util/store_sqlite.*). Built as its own tiny static library so the
# single ~250k-line generated .c file only ever gets compiled once.
# ---------------------------------------------------------------------------
add_library(sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC third_party/sqlite3)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION      # config db never loads extensions; smaller attack surface
    SQLITE_DQS=0                    # reject double-quoted string literals (modern, safer default)
)
target_link_libraries(sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if (NOT MSVC)
    target_link_libraries(sqlite3 PRIVATE m)
endif()

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(bowsers_image_viewer
    src/main.cpp
    src/ui/app.cpp
    src/ui/folder_model.cpp
    src/ui/image_loader.cpp
    src/ui/video_controller.cpp   # compiles to a no-op stub when BIV_ENABLE_VIDEO=OFF
    src/platform/windows_file_association.cpp
    src/platform/windows_console.cpp
    src/platform/windows_argv.cpp
    src/platform/windows_folder_sort.cpp
    src/ui/sidebar/metadata.cpp
    src/image/image_document.cpp
    src/image/decode.cpp
    src/image/thumbnail.cpp
    src/image/box_downscaler.cpp
    src/image/resize.cpp
    src/filesystem/directory_model.cpp
    src/filesystem/archive_source.cpp
    src/util/store_sqlite.cpp
)

if (MINGW)
    target_link_libraries(bowsers_image_viewer PRIVATE stdc++exp)
endif()

set_property(TARGET bowsers_image_viewer PROPERTY SLINT_EMBED_RESOURCES embed-files)

slint_target_sources(bowsers_image_viewer ui/app.slint)

target_compile_features(bowsers_image_viewer PRIVATE cxx_std_23)

target_include_directories(bowsers_image_viewer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/portable-file-dialogs
    ${stb_SOURCE_DIR}
)

target_link_libraries(bowsers_image_viewer PRIVATE Slint::Slint Threads::Threads clip sqlite3)

if(WIN32)
    target_sources(bowsers_image_viewer PRIVATE assets/bowsers_image_viewer.rc)
    # shell32: file associations (windows_file_association.cpp) and the
    # shellbag PIDL helpers windows_folder_sort.cpp reads Explorer's
    # per-folder sort with. advapi32: the registry calls both of those use;
    # normally pulled in by default anyway, but linked explicitly since we
    # now depend on it directly rather than just transitively.
    target_link_libraries(bowsers_image_viewer PRIVATE shell32 advapi32)

    # GUI subsystem: no terminal window on launch. MinGW gets -mwindows (its CRT
    # still calls main()); MSVC needs the entry point pointed at main() explicitly.
    # platform::attach_parent_console() restores --help/--version output when the
    # app is started from a terminal. Configure with -DBIV_WINDOWS_CONSOLE=ON to
    # get a normal console app back.
    if(NOT BIV_WINDOWS_CONSOLE)
        set_target_properties(bowsers_image_viewer PROPERTIES WIN32_EXECUTABLE TRUE)
        if(MSVC)
            target_link_options(bowsers_image_viewer PRIVATE /ENTRY:mainCRTStartup)
        endif()
    endif()
endif()
target_include_directories(bowsers_image_viewer PRIVATE ${LIBEXIF_INCLUDE_DIRS})
target_link_libraries(bowsers_image_viewer PRIVATE ${LIBEXIF_LIBRARIES})

if(BIV_ENABLE_ARCHIVES)
    target_compile_definitions(bowsers_image_viewer PRIVATE BIV_ENABLE_ARCHIVES=1)
    target_include_directories(bowsers_image_viewer PRIVATE ${LibArchive_INCLUDE_DIRS})
    target_link_libraries(bowsers_image_viewer PRIVATE ${LibArchive_LIBRARIES})
endif()

if(BIV_ENABLE_AVIF)
    target_compile_definitions(bowsers_image_viewer PRIVATE BIV_ENABLE_AVIF=1)
    target_include_directories(bowsers_image_viewer PRIVATE ${AVIF_INCLUDE_DIRS})
    target_link_directories(bowsers_image_viewer PRIVATE ${AVIF_LIBRARY_DIRS})
    target_link_libraries(bowsers_image_viewer PRIVATE ${AVIF_LIBRARIES})
endif()

if(BIV_ENABLE_WEBP)
    target_compile_definitions(bowsers_image_viewer PRIVATE BIV_ENABLE_WEBP=1)
    target_include_directories(bowsers_image_viewer PRIVATE ${WEBP_INCLUDE_DIRS})
    target_link_directories(bowsers_image_viewer PRIVATE ${WEBP_LIBRARY_DIRS})
    target_link_libraries(bowsers_image_viewer PRIVATE ${WEBP_LIBRARIES})
endif()

if(BIV_ENABLE_JPEG)
    target_sources(bowsers_image_viewer PRIVATE src/image/jpeg_scaled.cpp)
    target_compile_definitions(bowsers_image_viewer PRIVATE BIV_ENABLE_JPEG=1)
    target_link_libraries(bowsers_image_viewer PRIVATE JPEG::JPEG)
endif()

if(BIV_ENABLE_PNG)
    target_sources(bowsers_image_viewer PRIVATE src/image/png_scaled.cpp)
    target_compile_definitions(bowsers_image_viewer PRIVATE BIV_ENABLE_PNG=1)
    target_link_libraries(bowsers_image_viewer PRIVATE PNG::PNG)
endif()

if(BIV_ENABLE_SVG)
    target_sources(bowsers_image_viewer PRIVATE src/image/svg_scaled.cpp)
    target_compile_definitions(bowsers_image_viewer PRIVATE BIV_ENABLE_SVG=1)
    target_link_libraries(bowsers_image_viewer PRIVATE lunasvg)
endif()

if(BIV_ENABLE_VIDEO)
    target_sources(bowsers_image_viewer PRIVATE src/video/video_player.cpp)
    target_compile_definitions(bowsers_image_viewer PRIVATE BIV_ENABLE_VIDEO=1)
    target_include_directories(bowsers_image_viewer SYSTEM PRIVATE ${miniaudio_SOURCE_DIR})
    target_link_libraries(bowsers_image_viewer PRIVATE PkgConfig::FFMPEG ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(bowsers_image_viewer PRIVATE /W4)
else()
    target_compile_options(bowsers_image_viewer PRIVATE -Wall -Wextra)
endif()

install(TARGETS bowsers_image_viewer RUNTIME DESTINATION bin)

if(UNIX AND NOT APPLE AND BIV_ENABLE_LINUX_DESKTOP_INTEGRATION)
    install(FILES
        ui/icon.png
        DESTINATION share/icons/hicolor/512x512/apps
        RENAME bowsers_image_viewer.png
    )
    install(FILES
        share/applications/bowsers_image_viewer.desktop
        DESTINATION share/applications
    )
endif()
