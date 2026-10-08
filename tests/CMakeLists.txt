# Replays an input script and writes the state of every tick in the format of
# the oracle (tests/oracle/record.h).
add_executable(replay replay.c)
target_link_libraries(replay PRIVATE ddnet_physics)

# Compares two state dumps bit for bit.
add_executable(dump_diff dump_diff.c)

add_executable(test_world_copy test_world_copy.c)
target_link_libraries(test_world_copy PRIVATE ddnet_physics)

# Lists the tiles of a map that are worth visiting, for the script generator.
add_executable(map_points map_points.c)
target_link_libraries(map_points PRIVATE ddnet_physics)

# The tests need a map. By default the Tutorial map of a DDNet checkout in
# ddnet/ is used, point DDNET_PHYSICS_TEST_MAP somewhere else if there is none.
set(DDNET_PHYSICS_TEST_MAP "${PROJECT_SOURCE_DIR}/ddnet/data/maps/Tutorial.map" CACHE FILEPATH
    "Map the tests run on")
if(EXISTS "${DDNET_PHYSICS_TEST_MAP}")
  add_test(NAME world_copy COMMAND test_world_copy "${DDNET_PHYSICS_TEST_MAP}")
  # Bit-identity against the real server, if the oracle was built
  # (tests/oracle/build.sh).
  if(EXISTS "${PROJECT_SOURCE_DIR}/build/oracle/build/DDNet-Server")
    add_test(NAME oracle
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/oracle/run_suite.sh" "${DDNET_PHYSICS_TEST_MAP}")
    set_tests_properties(oracle PROPERTIES
      ENVIRONMENT "DDNET_PHYSICS_BUILD=${PROJECT_BINARY_DIR}" TIMEOUT 1800)
  endif()
else()
  message(STATUS "ddnet_physics: no test map at ${DDNET_PHYSICS_TEST_MAP}, tests are not registered")
endif()

# Ticks per second with random inputs.
add_executable(bench bench.c)
target_link_libraries(bench PRIVATE ddnet_physics)
# (the run of --tas is one through this map)
target_compile_definitions(bench PRIVATE BENCH_TAS_MAP="${CMAKE_CURRENT_SOURCE_DIR}/Aip-Gores.map")

# ddnet_world_copy() of the optimized backend only copies what changed. This
# builds that backend once more, with every such copy checked against a full
# comparison, and copies worlds around between ticks.
if(DDNET_PHYSICS_BACKEND STREQUAL "optimized")
  add_executable(copy_stress copy_stress.c
    ${PROJECT_SOURCE_DIR}/src/common/config.c
    ${PROJECT_SOURCE_DIR}/src/common/console.c
    ${PROJECT_SOURCE_DIR}/src/common/tuning.c)
  target_include_directories(copy_stress PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_compile_definitions(copy_stress PRIVATE
    DDNET_PHYSICS_CHECK_COPY DDNET_PHYSICS_BACKEND_OPTIMIZED)
  target_link_libraries(copy_stress PRIVATE ddnet_map_loader ddnet_physics_options m)
  set_target_properties(copy_stress PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  if(EXISTS "${DDNET_PHYSICS_TEST_MAP}")
    add_test(NAME copy_stress_1 COMMAND copy_stress "${DDNET_PHYSICS_TEST_MAP}" 1 600)
    add_test(NAME copy_stress_8 COMMAND copy_stress "${DDNET_PHYSICS_TEST_MAP}" 8 600)
  endif()
endif()
