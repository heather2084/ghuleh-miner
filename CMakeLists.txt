# Default to clang-16, matching the Makefile (`ifeq ($(origin CC), default)`).
# Must run before project() — that's what triggers compiler detection/caching.
# A user-supplied `-DCMAKE_C_COMPILER=...`/`-DCMAKE_CXX_COMPILER=...` already
# populates the cache before this script runs, so the NOT-DEFINED guards below
# only fire on the "nothing specified" default path; without them, CMake picks
# whatever `cc`/`c++` resolves to on $PATH (e.g. stock GCC), which fails to
# build clhash_native.h's Clang-only vector-literal syntax.
if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang-16 CACHE STRING "C compiler")
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++-16 CACHE STRING "C++ compiler")
endif()

cmake_minimum_required(VERSION 3.10)
project(ghuleh-miner VERSION 1.1.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(PRIMO_LINKER "lld" CACHE STRING "Linker driver name for -fuse-ld=<name>; set empty to use the compiler default")

# Device profile selector — mirrors the Makefile's PROFILE variable. The CLHash
# hand-asm is now runtime-dispatched (both variants compiled in, selected per
# core — see clhash_native.h), so the profile no longer gates it; it only
# toggles the Cortex-A53 erratum workaround:
#   rk3588  (default) — adds -mfix-cortex-a53-835769 (harmless NOP elsewhere).
#   generic           — drops it (no real A53 core):
#                         cmake -DPRIMO_PROFILE=generic ..
# Both produce the same runtime-dispatched binary otherwise.
set(PRIMO_PROFILE "rk3588" CACHE STRING "Device profile: rk3588 or generic")

# Match the promoted ccminer baseline profile on this device.
# The validated winner tuned for A55/A53-style codegen, not the earlier A76 profile.
set(PRIMO_BASE_ARCH_FLAGS "-march=armv8-a+crypto -mtune=cortex-a53")
set(PRIMO_COMMON_FLAGS "-O3 -ffinite-loops -ffast-math -D_REENTRANT -DUSE_DIRECT_NATIVE_CALL=1 -falign-functions=16 -fomit-frame-pointer -fpic -pthread -flto -fno-stack-protector -flax-vector-conversions -Wall")
if(PRIMO_PROFILE STREQUAL "generic")
    # The CLHash hand-asm is no longer profile-gated: it is compiled into both
    # clhash_native.c (_asm) and clhash_native_noasm.c (_noasm) and selected
    # per-thread at runtime by core type (see clhash_native.h). Nothing to add.
elseif(PRIMO_PROFILE STREQUAL "rk3588")
    string(APPEND PRIMO_COMMON_FLAGS " -mfix-cortex-a53-835769")
else()
    message(FATAL_ERROR "Unknown PRIMO_PROFILE '${PRIMO_PROFILE}' (expected rk3588 or generic)")
endif()
set(PRIMO_C_ONLY_FLAGS "")
set(PRIMO_SCRYPT_NOSLP_FLAG "-fno-tree-slp-vectorize")
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(PRIMO_C_ONLY_FLAGS "-mllvm -enable-loop-distribute")
    set(PRIMO_SCRYPT_NOSLP_FLAG "-fno-slp-vectorize")
endif()
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${PRIMO_BASE_ARCH_FLAGS} ${PRIMO_COMMON_FLAGS} ${PRIMO_C_ONLY_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${PRIMO_BASE_ARCH_FLAGS} ${PRIMO_COMMON_FLAGS} -funroll-loops")

# Find required libraries
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# Find jansson (JSON library)
find_library(JANSSON_LIBRARY NAMES jansson)
find_path(JANSSON_INCLUDE_DIR jansson.h)

if(NOT JANSSON_LIBRARY OR NOT JANSSON_INCLUDE_DIR)
    message(FATAL_ERROR "jansson library not found. Install with: sudo apt-get install libjansson-dev")
endif()

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/yespower
    ${CURL_INCLUDE_DIRS}
    ${JANSSON_INCLUDE_DIR}
)

# Source files
set(SOURCES
    src/main.cpp
    src/config.cpp
    src/api.cpp
    src/miner.cpp
    src/dev_fee.cpp
    src/stratum.cpp
    src/stratum_state.cpp
    src/stratum_job.cpp
    src/stratum_rpc.cpp
    src/stratum_handshake.cpp
    src/stratum_session.cpp
    src/stratum_transport.cpp
    src/stratum_standard.cpp
    src/stratum_verus.cpp
    src/algorithm/verus.cpp
    src/algorithm/clhash_native.c
    src/algorithm/clhash_native_noasm.c
    src/algorithm/haraka_native.c
    src/algorithm/cpu_features.c
    src/algorithm/sha256_neon.c
    src/algorithm/scrypt_neon.c
    src/algorithm/sha256_ce_asm.S
    src/algorithm/scrypt_blockmix_asm.S
    src/algorithm/civiclight.cpp
    third_party/yespower/yespower-opt.c
    third_party/yespower/sha256.c
    src/utils/log.cpp
    src/utils/dns_fallback.c
)

# RandomX (Monero). The vendored library builds through its OWN CMakeLists so
# our -ffast-math never touches its consensus-critical IEEE-754 code (wrong
# rounding = wrong hashes). CMAKE_C/CXX_FLAGS are inherited by add_subdirectory,
# so we blank our aggressive flags for the descent and restore them after — the
# subdir gets a clean -O3 (Release) + its own -march=armv8-a+crypto.
# -DPRIMO_RANDOMX=0 opts out and drops the algorithm.
option(PRIMO_RANDOMX "Build RandomX (Monero) support" ON)
if(PRIMO_RANDOMX)
    list(APPEND SOURCES
        src/algorithm/randomx_algo.cpp
        src/stratum_xmr.cpp
    )
    set(_primo_saved_c_flags "${CMAKE_C_FLAGS}")
    set(_primo_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
    set(CMAKE_C_FLAGS "${PRIMO_BASE_ARCH_FLAGS}")
    set(CMAKE_CXX_FLAGS "${PRIMO_BASE_ARCH_FLAGS}")
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release)
    endif()
    add_subdirectory(third_party/RandomX EXCLUDE_FROM_ALL)
    set(CMAKE_C_FLAGS "${_primo_saved_c_flags}")
    set(CMAKE_CXX_FLAGS "${_primo_saved_cxx_flags}")
endif()

# Executable
add_executable(ghuleh-miner ${SOURCES})

if(PRIMO_RANDOMX)
    target_compile_definitions(ghuleh-miner PRIVATE PRIMO_RANDOMX=1)
    target_include_directories(ghuleh-miner PRIVATE third_party/RandomX/src)
endif()

# Match Makefile behavior for per-file hot-path tuning.
set_source_files_properties(
    src/algorithm/scrypt_neon.c
    PROPERTIES COMPILE_OPTIONS "${PRIMO_SCRYPT_NOSLP_FLAG}"
)
set_source_files_properties(
    src/algorithm/clhash_native.c
    src/algorithm/clhash_native_noasm.c
    src/algorithm/haraka_native.c
    PROPERTIES COMPILE_OPTIONS "-fno-unroll-loops"
)

# Link libraries
target_link_libraries(ghuleh-miner
    ${CURL_LIBRARIES}
    ${JANSSON_LIBRARY}
    Threads::Threads
    m
)

if(PRIMO_RANDOMX)
    target_link_libraries(ghuleh-miner randomx)
endif()

# Match the promoted ccminer baseline linker shape.
if(PRIMO_LINKER)
    target_link_options(ghuleh-miner PRIVATE "-flto" "-fuse-ld=${PRIMO_LINKER}")
else()
    target_link_options(ghuleh-miner PRIVATE "-flto")
endif()

# Installation
install(TARGETS ghuleh-miner DESTINATION bin)
