add_executable(vapor src/main.c)
target_link_libraries(vapor PRIVATE vapor_core)
target_compile_options(vapor PRIVATE ${VAPOR_WARN_FLAGS})

install(TARGETS vapor RUNTIME DESTINATION bin)
