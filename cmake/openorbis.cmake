# Toolchain file for cross-compiling to PS4 with OpenOrbis.
# Based on bucanero/SDL-PS4's openorbis.cmake, adapted for the official
# v0.5.4 release (host packaging tools under bin/<host-os>, system clang).
#
# Usage: cmake -B build-ps4 -DCMAKE_TOOLCHAIN_FILE=cmake/openorbis.cmake
# Paths: OO_PS4_TOOLCHAIN, else OO_PS4_TOOLCHAIN_SRC on Windows (same SDK root)

cmake_minimum_required(VERSION 3.16)

if (DEFINED ENV{OO_PS4_TOOLCHAIN})
    set(OPENORBIS $ENV{OO_PS4_TOOLCHAIN})
elseif (CMAKE_HOST_WIN32 AND DEFINED ENV{OO_PS4_TOOLCHAIN_SRC}
        AND EXISTS "$ENV{OO_PS4_TOOLCHAIN_SRC}/link.x")
    # OO_PS4_TOOLCHAIN_SRC=C:\OpenOrbis\PS4Toolchain
    set(OPENORBIS "$ENV{OO_PS4_TOOLCHAIN_SRC}")
elseif (EXISTS "$ENV{HOME}/ps4dev/OpenOrbis/PS4Toolchain")
    set(OPENORBIS "$ENV{HOME}/ps4dev/OpenOrbis/PS4Toolchain")
else ()
    message(FATAL_ERROR "Set OO_PS4_TOOLCHAIN to the OpenOrbis SDK path, (on Windows, OO_PS4_TOOLCHAIN_SRC is also honoured)")
endif ()
set(OO_PS4_TOOLCHAIN ${OPENORBIS})

set(PS4 TRUE)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_SYSTEM_VERSION 12)
set(CMAKE_CROSSCOMPILING 1)

# System clang/lld; packaging tools live under a per-host subdir.
if (CMAKE_HOST_WIN32)
    set(OO_HOST_TOOLS ${OPENORBIS}/bin/windows)
    set(OO_EXE_SUFFIX ".exe")
elseif (APPLE)
    set(OO_HOST_TOOLS ${OPENORBIS}/bin/macos)
    set(OO_EXE_SUFFIX "")
else ()
    set(OO_HOST_TOOLS ${OPENORBIS}/bin/linux)
    set(OO_EXE_SUFFIX "")
endif ()

set(_OO_LLVM_HINTS "")
if (CMAKE_HOST_WIN32)
    list(APPEND _OO_LLVM_HINTS "C:/Program Files/LLVM/bin" "C:/Program Files (x86)/LLVM/bin")
endif ()

find_program(CMAKE_C_COMPILER NAMES clang PATHS ${_OO_LLVM_HINTS} NO_DEFAULT_PATH)
if (NOT CMAKE_C_COMPILER)
    find_program(CMAKE_C_COMPILER NAMES clang REQUIRED)
endif ()
find_program(CMAKE_CXX_COMPILER NAMES clang++ PATHS ${_OO_LLVM_HINTS} NO_DEFAULT_PATH)
if (NOT CMAKE_CXX_COMPILER)
    find_program(CMAKE_CXX_COMPILER NAMES clang++ REQUIRED)
endif ()
find_program(CMAKE_ASM_COMPILER NAMES clang PATHS ${_OO_LLVM_HINTS} NO_DEFAULT_PATH)
if (NOT CMAKE_ASM_COMPILER)
    find_program(CMAKE_ASM_COMPILER NAMES clang REQUIRED)
endif ()
find_program(CMAKE_LINKER NAMES ld.lld PATHS ${_OO_LLVM_HINTS} NO_DEFAULT_PATH)
if (NOT CMAKE_LINKER)
    find_program(CMAKE_LINKER NAMES ld.lld REQUIRED)
endif ()
find_program(CMAKE_AR NAMES llvm-ar PATHS ${_OO_LLVM_HINTS} NO_DEFAULT_PATH)
if (NOT CMAKE_AR)
    find_program(CMAKE_AR NAMES llvm-ar REQUIRED)
endif ()
find_program(CMAKE_RANLIB NAMES llvm-ranlib PATHS ${_OO_LLVM_HINTS} NO_DEFAULT_PATH)
if (NOT CMAKE_RANLIB)
    find_program(CMAKE_RANLIB NAMES llvm-ranlib REQUIRED)
endif ()
unset(_OO_LLVM_HINTS)

set(CMAKE_FIND_ROOT_PATH ${OPENORBIS} ${OPENORBIS}/usr)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "No shared libraries on PS4")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# CMake's compiler test cannot link a full PS4 executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(PS4_COMMON_INCLUDES "-isysroot ${OPENORBIS} -isystem ${OPENORBIS}/include")
set(PS4_COMMON_FLAGS "--target=x86_64-pc-freebsd12-elf -D__PS4__ -D__OPENORBIS__ -D__ORBIS__ -fPIC -funwind-tables ${PS4_COMMON_INCLUDES}")

set(CMAKE_C_FLAGS_INIT "${PS4_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PS4_COMMON_FLAGS} -isystem ${OPENORBIS}/include/c++/v1")
set(CMAKE_ASM_FLAGS_INIT "${PS4_COMMON_FLAGS}")

# Direct link with ld.lld: Sony script, PIE, and crt1.
set(PS4_LINKER_FLAGS "-m elf_x86_64 -pie --eh-frame-hdr --script ${OPENORBIS}/link.x ${OPENORBIS}/lib/crt1.o -L${OPENORBIS}/lib")
set(PS4_BASE_LIBS "-lc -lkernel")

set(CMAKE_EXE_LINKER_FLAGS_INIT "${PS4_LINKER_FLAGS}")
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_LINKER> -o <TARGET> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES> ${PS4_BASE_LIBS}")
set(CMAKE_CXX_LINK_EXECUTABLE "<CMAKE_LINKER> -o <TARGET> <LINK_FLAGS> <OBJECTS> <LINK_LIBRARIES> ${PS4_BASE_LIBS} -lc++")
