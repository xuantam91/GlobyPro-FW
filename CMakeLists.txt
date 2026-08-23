# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.1.6")

# set(CMAKE_CXX_STANDARD 17)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

if(NOT DEFINED JIUCHUAN_DEFAULT_LCD_DRIVER)
    set(JIUCHUAN_DEFAULT_LCD_DRIVER 2)
endif()
add_compile_definitions(JIUCHUAN_DEFAULT_LCD_DRIVER=${JIUCHUAN_DEFAULT_LCD_DRIVER})

if(NOT DEFINED DEFAULT_THEME_VAL)
    set(DEFAULT_THEME_VAL 1)
endif()
add_compile_definitions(DEFAULT_THEME_VAL=${DEFAULT_THEME_VAL})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi_vn)
