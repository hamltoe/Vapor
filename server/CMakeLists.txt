find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
find_package(Threads REQUIRED)

# Shared by vapord and vapor-admin: config, database, password hashing,
# content-root path building, and Steam metadata lookup.

add_library(vapord_core STATIC
    src/config.c
    src/db.c
    src/auth.c
    src/content.c
    src/discover.c
    src/meta.c
)
target_include_directories(vapord_core PUBLIC src)
target_link_libraries(vapord_core PUBLIC
    vapor_common
    vapor_sqlite
    vapor_miniz
    PkgConfig::SODIUM
    PkgConfig::CURL
    Threads::Threads
)
target_compile_options(vapord_core PRIVATE ${VAPOR_WARN_FLAGS})

add_executable(vapord
    src/main.c
    src/http.c
    src/routes_auth.c
    src/routes_catalog.c
    src/routes_download.c
    src/gui.c
)
target_link_libraries(vapord PRIVATE
    vapord_core
    vapor_civetweb
    vapor_nuklear
    vapor_sdl2
)
target_compile_options(vapord PRIVATE
    ${VAPOR_WARN_FLAGS} -Wno-unused-function -Wno-sign-compare
)

add_executable(vapor-admin tools/vapor_admin.c)
target_link_libraries(vapor-admin PRIVATE vapord_core)
target_compile_options(vapor-admin PRIVATE ${VAPOR_WARN_FLAGS})

install(TARGETS vapord vapor-admin RUNTIME DESTINATION bin)
