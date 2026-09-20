add_library(vapor_common STATIC
    src/buf.c
    src/sha256.c
    src/util.c
    src/manifest.c
    src/iso9660.c
    src/wise.c
)

target_include_directories(vapor_common PUBLIC include)
target_link_libraries(vapor_common PUBLIC vapor_cjson vapor_miniz)
target_compile_options(vapor_common PRIVATE ${VAPOR_WARN_FLAGS})
set_target_properties(vapor_common PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Self-test for the pieces where a silent bug would corrupt installs: the
# SHA-256 implementation, ISO/Wise unpackers, and the version/glob/id helpers.
add_executable(vapor-selftest tests/selftest.c)
target_link_libraries(vapor-selftest PRIVATE vapor_common)
target_compile_options(vapor-selftest PRIVATE ${VAPOR_WARN_FLAGS})

enable_testing()
add_test(NAME common_selftest COMMAND vapor-selftest)
