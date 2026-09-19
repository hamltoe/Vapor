add_subdirectory(core)
add_subdirectory(cli)

if(VAPOR_BUILD_GUI)
    add_subdirectory(gui)
endif()
