cmake_minimum_required(VERSION 3.16)
project(meshvpn VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Default build type ────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type: Release | Debug | RelWithDebInfo | MinSizeRel" FORCE)
endif()

# No -march=native by default — binary runs on any x86-64.
# Enable with -DMESHVPN_NATIVE=ON
set(CMAKE_CXX_FLAGS_RELEASE        "-O3 -flto -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG          "-g -O0 -fsanitize=address,undefined")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_MINSIZEREL     "-Os -DNDEBUG")

# =============================================================================
# OPTIONS — set with:  cmake .. -D<OPTION>=ON/OFF
# =============================================================================

# ── Feature flags ─────────────────────────────────────────────────────────────
option(MESHVPN_XDP
    "AF_XDP zero-copy path (kernel >= 5.1 + XDP NIC + libbpf)" OFF)

option(MESHVPN_SODIUM
    "Real crypto: libsodium ChaCha20-Poly1305 instead of XOR placeholder" OFF)

# ── Compiler tuning ───────────────────────────────────────────────────────────
option(MESHVPN_NATIVE
    "Add -march=native (faster on this machine, may crash on others)" OFF)

option(MESHVPN_LTO
    "Link-time optimisation (ON for Release builds)" ON)

option(MESHVPN_ASAN
    "Force address+UB sanitisers even in Release (for CI)" OFF)

# =============================================================================
# Status output
# =============================================================================
message(STATUS "")
message(STATUS "╔══════════════════════════════════════════╗")
message(STATUS "║       MeshVPN Build Configuration       ║")
message(STATUS "╚══════════════════════════════════════════╝")
message(STATUS "  CMAKE_BUILD_TYPE : ${CMAKE_BUILD_TYPE}")
message(STATUS "  MESHVPN_XDP      : ${MESHVPN_XDP}")
message(STATUS "  MESHVPN_SODIUM   : ${MESHVPN_SODIUM}")
message(STATUS "  MESHVPN_NATIVE   : ${MESHVPN_NATIVE}")
message(STATUS "  MESHVPN_LTO      : ${MESHVPN_LTO}")
message(STATUS "  MESHVPN_ASAN     : ${MESHVPN_ASAN}")
message(STATUS "")
message(STATUS "  Source:  meshvpn.cpp  (server + client in one binary)")
message(STATUS "")

# =============================================================================
# Helpers
# =============================================================================

function(meshvpn_common tgt)
    target_compile_options(${tgt} PRIVATE
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-unused-result
    )
    if(MESHVPN_NATIVE)
        target_compile_options(${tgt} PRIVATE -march=native)
        message(STATUS "[${tgt}] -march=native ON")
    endif()
    if(MESHVPN_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
        set_property(TARGET ${tgt} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    if(MESHVPN_ASAN)
        target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined)
        target_link_options(${tgt}    PRIVATE -fsanitize=address,undefined)
    endif()
    target_link_libraries(${tgt} PRIVATE pthread)
endfunction()

function(meshvpn_sodium tgt)
    if(MESHVPN_SODIUM)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SODIUM REQUIRED libsodium)
        target_compile_definitions(${tgt} PRIVATE WITH_SODIUM)
        target_include_directories(${tgt} PRIVATE ${SODIUM_INCLUDE_DIRS})
        target_link_libraries(${tgt}      PRIVATE ${SODIUM_LIBRARIES})
        message(STATUS "[${tgt}] crypto: libsodium ChaCha20-Poly1305")
    else()
        message(STATUS "[${tgt}] crypto: XOR placeholder  (use -DMESHVPN_SODIUM=ON for real crypto)")
    endif()
endfunction()

function(meshvpn_xdp tgt)
    if(MESHVPN_XDP)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LIBBPF REQUIRED libbpf)
        target_compile_definitions(${tgt} PRIVATE WITH_XDP)
        target_include_directories(${tgt} PRIVATE ${LIBBPF_INCLUDE_DIRS})
        target_link_libraries(${tgt}      PRIVATE ${LIBBPF_LIBRARIES})
        target_sources(${tgt}             PRIVATE xdp_sock.cpp)

        # Older system kernel headers predate `enum bpf_stats_type`, which
        # libbpf's userspace bpf.h forward-declares -- a hard C++ error
        # without a full definition (xdp_sock.cpp supplies one as a
        # fallback). Newer kernel headers already define it, where
        # supplying it too would itself be a duplicate-definition error --
        # so probe for which case this build is in.
        set(CMAKE_REQUIRED_INCLUDES ${LIBBPF_INCLUDE_DIRS})
        include(CheckCXXSourceCompiles)
        check_cxx_source_compiles("
            #include <linux/bpf.h>
            int main() { return BPF_STATS_RUN_TIME; }
        " MESHVPN_KERNEL_HEADERS_HAVE_BPF_STATS_TYPE)
        unset(CMAKE_REQUIRED_INCLUDES)
        if(NOT MESHVPN_KERNEL_HEADERS_HAVE_BPF_STATS_TYPE)
            target_compile_definitions(${tgt} PRIVATE MESHVPN_NEED_BPF_STATS_TYPE_SHIM)
        endif()

        find_program(CLANG_BPF clang REQUIRED)
        # Detect arch-specific include path (e.g. x86_64-linux-gnu, aarch64-linux-gnu)
        execute_process(
            COMMAND dpkg-architecture -qDEB_HOST_MULTIARCH
            OUTPUT_VARIABLE DEB_HOST_MULTIARCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT DEB_HOST_MULTIARCH)
            set(DEB_HOST_MULTIARCH "x86_64-linux-gnu") # fallback
        endif()
        message(STATUS "[${tgt}] BPF arch include: /usr/include/${DEB_HOST_MULTIARCH}")

        add_custom_command(
            OUTPUT  ${CMAKE_BINARY_DIR}/xdp_kern.o
            COMMAND ${CLANG_BPF}
                    -O2 -g -target bpf
                    -I/usr/include
                    -I/usr/include/${DEB_HOST_MULTIARCH}
                    -I${CMAKE_SOURCE_DIR}
                    -c ${CMAKE_SOURCE_DIR}/xdp_kern.c
                    -o ${CMAKE_BINARY_DIR}/xdp_kern.o
            DEPENDS ${CMAKE_SOURCE_DIR}/xdp_kern.c
            COMMENT "Compiling BPF XDP kernel program (arch: ${DEB_HOST_MULTIARCH})"
        )
        add_custom_target(xdp_kern DEPENDS ${CMAKE_BINARY_DIR}/xdp_kern.o)
        add_dependencies(${tgt} xdp_kern)
        install(FILES ${CMAKE_BINARY_DIR}/xdp_kern.o DESTINATION bin)
        message(STATUS "[${tgt}] AF_XDP: enabled  (libbpf ${LIBBPF_VERSION})")
    else()
        message(STATUS "[${tgt}] AF_XDP: OFF  (use -DMESHVPN_XDP=ON for zero-copy)")
    endif()
endfunction()

# =============================================================================
# TARGET: meshvpn
# Single binary — contains everything:
#   --mode server  →  pure bridge/router, no TUN
#   --mode client  →  VPN client with TUN, relay or P2P comm
# =============================================================================
add_executable(ReVPN-engine meshvpn.cpp)
meshvpn_common(ReVPN-engine)
meshvpn_sodium(ReVPN-engine)
meshvpn_xdp(ReVPN-engine)
install(TARGETS ReVPN-engine DESTINATION bin)

# =============================================================================
# Final summary
# =============================================================================
message(STATUS "")
message(STATUS "Build:    cmake --build . -j\$(nproc)")
message(STATUS "Install:  sudo cmake --install .")
message(STATUS "")