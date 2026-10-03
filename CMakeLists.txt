cmake_minimum_required(VERSION 3.16)
project(Vapor
        VERSION 0.1.0
        DESCRIPTION "Self-hosted game install manager"
        LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # civetweb and our POSIX code want _GNU_SOURCE-era decls

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${cfg}" cfg_up)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_up} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# The server needs civetweb plus libsodium and is only ever deployed on Linux,
# so it is off by default on Windows where libsodium is not available.
if(UNIX)
    set(_vapor_server_default ON)
else()
    set(_vapor_server_default OFF)
endif()

option(VAPOR_BUILD_SERVER "Build vapord and vapor-admin" ${_vapor_server_default})
option(VAPOR_BUILD_CLIENT "Build libvapor and the vapor CLI" ON)
option(VAPOR_BUILD_GUI    "Build SDL2/Nuklear windows (vapord host UI and vapor-gui)" ON)
# Separate from the VS 2022 x64 client. scripts/build-windows-xp.ps1 turns this
# on with MinGW-w64 i686. The Universal CRT from VS 2022 does not run on XP.
option(VAPOR_TARGET_XP "Build a Windows XP SP3 32-bit client" OFF)
set(VAPOR_XP_SDL_ROOT "" CACHE PATH "Install prefix of SDL 2.0.22 for the XP GUI")

if(VAPOR_BUILD_SERVER AND NOT VAPOR_BUILD_GUI)
    message(FATAL_ERROR
        "vapord always presents a host window, so VAPOR_BUILD_GUI must be ON "
        "when building the server. Install SDL2 (scripts/bootstrap-linux.sh). "
        "Runtime --headless is for tests and systemd, not a separate binary.")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/sqlite3.c")
    message(FATAL_ERROR
        "third_party/ is empty. Run scripts/vendor-deps.ps1 (Windows) or "
        "scripts/vendor-deps.sh (Linux) first.")
endif()

# Warning flags applied to our code only; third_party gets them switched off.
if(VAPOR_TARGET_XP)
    if(NOT WIN32)
        message(FATAL_ERROR "VAPOR_TARGET_XP is only for the Windows client")
    endif()
    if(NOT MINGW)
        message(FATAL_ERROR
            "VAPOR_TARGET_XP needs MinGW-w64 i686. Visual Studio's CRT does not run on Windows XP.")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR
            "VAPOR_TARGET_XP needs a 32-bit compiler (MinGW-w64 i686).")
    endif()
    # 0x0501 keeps headers from exposing Vista imports. Subsystem 5.01 is what
    # the XP loader checks; a 6.0 image is rejected as not a valid Win32 program.
    add_compile_definitions(_WIN32_WINNT=0x0501 WINVER=0x0501 VAPOR_TARGET_XP=1)
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -Wl,--subsystem,console:5.01 -Wl,--major-os-version,5,--minor-os-version,1,--major-subsystem-version,5,--minor-subsystem-version,1")
    # This GCC is configured --with-arch=pentium4. An Athlon XP (the XP machines
    # this client is built for) has SSE and 3DNow!, not SSE2, and the loader
    # reports an SSE2 instruction as "The application failed to initialize
    # properly". Keep Vapor's own code on i686, and scalar, so SHA-256 matches
    # the server. The no-vectorize flags are repeated after -O3, which turns
    # -ftree-vectorize back on. The Win32 ABI only aligns the stack to 4 bytes.
    # VAPOR_XP_CRT_LIB is a mingw-w64 CRT rebuilt with the same flags. The stock
    # libmingwex uses SSE2 in stat and printf.
    set(VAPOR_XP_CRT_LIB "" CACHE PATH "lib directory of an i686 mingw-w64 CRT built without SSE2")
    if(VAPOR_XP_CRT_LIB)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -L${VAPOR_XP_CRT_LIB}")
    endif()
    set(VAPOR_XP_ARCH_FLAGS
        "-march=i686 -mtune=generic -mno-sse -mno-sse2 -mno-mmx -mfpmath=387 -mstackrealign -fno-tree-vectorize -fno-tree-slp-vectorize -fno-tree-loop-vectorize")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${VAPOR_XP_ARCH_FLAGS}")
    foreach(cfg RELEASE RELWITHDEBINFO MINSIZEREL DEBUG)
        set(CMAKE_C_FLAGS_${cfg} "${CMAKE_C_FLAGS_${cfg}} ${VAPOR_XP_ARCH_FLAGS}")
    endforeach()
endif()

if(MSVC)
    set(VAPOR_WARN_FLAGS /W4 /wd4996 /wd4200)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN)
else()
    set(VAPOR_WARN_FLAGS -Wall -Wextra -Wno-unused-parameter)
endif()

add_subdirectory(third_party)
add_subdirectory(common)

if(VAPOR_BUILD_SERVER)
    add_subdirectory(server)
endif()
if(VAPOR_BUILD_CLIENT)
    add_subdirectory(client)
endif()

message(STATUS "")
message(STATUS "Vapor ${PROJECT_VERSION}  (${CMAKE_BUILD_TYPE})")
message(STATUS "  server .... ${VAPOR_BUILD_SERVER}")
message(STATUS "  client .... ${VAPOR_BUILD_CLIENT}")
message(STATUS "  gui ....... ${VAPOR_BUILD_GUI}")
message(STATUS "  xp ........ ${VAPOR_TARGET_XP}")
message(STATUS "")
