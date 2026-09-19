# vapor-gui: the same libvapor the CLI links, behind a Nuklear + SDL2 shell.

add_executable(vapor-gui
    src/main.c
    src/screens.c
    src/worker.c
    src/covers.c
)

target_link_libraries(vapor-gui PRIVATE
    vapor_core
    vapor_nuklear
    vapor_sdl2
    vapor_stb
)
# Every file here sees nuklear.h, which is not warning-clean under our flags.
# Only the specific upstream diagnostics are switched off, so real warnings in
# our own code still show up.
if(MSVC)
    # 4116 unnamed type in parentheses, 5287 mixed enum operands, 4701/4703
    # maybe-uninitialized: all inside nuklear.h.
    target_compile_options(vapor-gui PRIVATE
        ${VAPOR_WARN_FLAGS} /wd4116 /wd5287 /wd4701 /wd4703)
else()
    target_compile_options(vapor-gui PRIVATE
        ${VAPOR_WARN_FLAGS} -Wno-unused-function -Wno-sign-compare)
endif()

if(WIN32 AND VAPOR_SDL2_DLL)
    add_custom_command(TARGET vapor-gui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${VAPOR_SDL2_DLL}" "$<TARGET_FILE_DIR:vapor-gui>"
        COMMENT "staging SDL2.dll next to vapor-gui")
endif()
