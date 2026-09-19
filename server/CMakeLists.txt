find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

# Shared by vapord and vapor-admin: config, database, password hashing, and
# content-root path building. No HTTP dependency.
add_library(vapord_core STATIC
    src/config.c
    src/db.c
    src/auth.c
    src/content.c
)
target_include_directories(vapord_core PUBLIC src)
target_link_libraries(vapord_core PUBLIC
    vapor_common
    vapor_sqlite
    PkgConfig::SODIUM
)
target_compile_options(vapord_core PRIVATE ${VAPOR_WARN_FLAGS})

add_executable(vapord
    src/main.c
    src/http.c
    src/routes_auth.c
    src/routes_catalog.c
    src/routes_download.c
)
target_link_libraries(vapord PRIVATE vapord_core vapor_civetweb)
target_compile_options(vapord PRIVATE ${VAPOR_WARN_FLAGS})

add_executable(vapor-admin tools/vapor_admin.c)
target_link_libraries(vapor-admin PRIVATE vapord_core vapor_miniz)
target_compile_options(vapor-admin PRIVATE ${VAPOR_WARN_FLAGS})

install(TARGETS vapord vapor-admin RUNTIME DESTINATION bin)
