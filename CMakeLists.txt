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
message(STATUS "")
