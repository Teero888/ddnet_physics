cmake_minimum_required(VERSION 3.16)
project(ddnet_physics_benchmarks LANGUAGES C)

# Copy the maps directory to the build directory
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/maps DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/..)

# generate the cmake files for the benchmarks
execute_process(COMMAND /usr/bin/bash ${CMAKE_CURRENT_SOURCE_DIR}/generate_cmake_template.sh)

add_subdirectory("global")
add_subdirectory("movebox")
add_subdirectory("test_box_character")
