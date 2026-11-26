# Works both as an ESP-IDF component and as a plain CMake library.
if(ESP_PLATFORM)
    # nothing but the C library is needed: the engine does no i/o, you give it a flush callback.
    # (ECCLES_GUI_EXEC_IRAM only needs esp_attr.h, which is part of the always available esp_common)
    idf_component_register(
        SRCS "src/eccles_gui.c"
        INCLUDE_DIRS "src" "config"
    )
else()
    cmake_minimum_required(VERSION 3.13)
    project(EcclesGUI VERSION 1.0.0 LANGUAGES C)

    add_library(eccles_gui src/eccles_gui.c)
    add_library(eccles::gui ALIAS eccles_gui)
    target_include_directories(eccles_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/config)
    target_compile_features(eccles_gui PUBLIC c_std_99)
    # no libm needed: the engine is integer only

    # options from eccles_gui_config.h can be passed here, e.g.
    #   cmake -DECCLES_GUI_DEFINES="ECCLES_GUI_NO_3D;ECCLES_GUI_MAX_WIDGETS=64" ..
    set(ECCLES_GUI_DEFINES "" CACHE STRING "extra compile definitions for eccles_gui, a ;-list")
    if(ECCLES_GUI_DEFINES)
        target_compile_definitions(eccles_gui PUBLIC ${ECCLES_GUI_DEFINES})
    endif()

    option(ECCLES_GUI_BUILD_TESTS "build the host tests" OFF)
    if(ECCLES_GUI_BUILD_TESTS)
        enable_testing()
        find_library(ECCLES_GUI_LIBM m)
        add_executable(test_gui tests/test_gui.c)
        target_link_libraries(test_gui PRIVATE eccles_gui ${ECCLES_GUI_LIBM})  # libm only for the sine table check in the test
        add_test(NAME eccles_gui COMMAND test_gui)
    endif()
endif()
