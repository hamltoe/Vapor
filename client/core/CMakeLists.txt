# libvapor: everything the CLI and the GUI share. The only platform-specific
# files are platform_* and net_*; the rest is portable C11.

set(VAPOR_CORE_SOURCES
    src/auth.c
    src/catalog.c
    src/config.c
    src/db.c
    src/json.c
    src/net.c
    src/install.c
    src/installer.c
    src/launch.c
)

if(WIN32)
    list(APPEND VAPOR_CORE_SOURCES src/platform_win32.c src/net_winhttp.c)
else()
    list(APPEND VAPOR_CORE_SOURCES src/platform_posix.c src/net_curl.c)
endif()

add_library(vapor_core STATIC ${VAPOR_CORE_SOURCES})

target_include_directories(vapor_core
    PUBLIC  include
    PRIVATE src
)
target_link_libraries(vapor_core PUBLIC
    vapor_common
    vapor_sqlite
    vapor_miniz
)
target_compile_options(vapor_core PRIVATE ${VAPOR_WARN_FLAGS})

if(WIN32)
    # winhttp and shell32/ole32 (SHGetKnownFolderPath) ship with the Windows
    # SDK, so the Windows client has no third-party runtime dependency.
    target_link_libraries(vapor_core PUBLIC winhttp shell32 ole32)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
    target_link_libraries(vapor_core PUBLIC PkgConfig::CURL)
endif()
