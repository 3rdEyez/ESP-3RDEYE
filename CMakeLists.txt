# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.2.10")
project(app)

# idf_ext.py rejects CLI writes before SDK serial selection. Also prevent direct
# generated build targets from invoking the unchecked full-flash arguments.
add_custom_target(satori_block_direct_flash
    COMMAND ${PYTHON} ${CMAKE_CURRENT_LIST_DIR}/tools/block_direct_flash.py
    VERBATIM)
foreach(write_target flash app-flash bootloader-flash partition-table-flash
        encrypted-flash encrypted-app-flash erase_flash erase-otadata)
    if(TARGET ${write_target})
        add_dependencies(${write_target} satori_block_direct_flash)
    endif()
endforeach()
