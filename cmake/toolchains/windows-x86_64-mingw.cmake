set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Set root to MSYS2 UCRT64 sysroot
set(BIV_MINGW_ROOT "/ucrt64" CACHE PATH "MinGW-w64 target sysroot")

# Compiler definitions (or leave unset if running directly within UCRT64 shell)
set(CMAKE_C_COMPILER "gcc" CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER "g++" CACHE FILEPATH "C++ compiler")
set(CMAKE_RC_COMPILER "windres" CACHE FILEPATH "Resource compiler")

if(EXISTS "${BIV_MINGW_ROOT}")
    set(CMAKE_FIND_ROOT_PATH "${BIV_MINGW_ROOT}")
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

    if(EXISTS "${BIV_MINGW_ROOT}/lib/pkgconfig")
        set(ENV{PKG_CONFIG_SYSROOT_DIR} "${BIV_MINGW_ROOT}")
        set(ENV{PKG_CONFIG_LIBDIR}
            "${BIV_MINGW_ROOT}/lib/pkgconfig:${BIV_MINGW_ROOT}/share/pkgconfig")
    endif()
endif()