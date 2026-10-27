# The tests of the Unique backends (see tests/CMakeLists.txt for the ones of
# DDNet).

# Replays an input script of the oracle and writes the state of every tick in
# the format of the oracle (oracle/record.h).
add_executable(replay replay.c)
target_link_libraries(replay PRIVATE ddnet_physics)

# Compares two state dumps bit for bit.
add_executable(dump_diff dump_diff.c)

# Lists the tiles of a map that are worth visiting, for the script generator.
add_executable(map_points map_points.c)
target_link_libraries(map_points PRIVATE ddnet_map_loader)

# Which maps have what the backends do not cover (ddnet_map_unsupported()).
add_executable(check_maps check_maps.c)
target_link_libraries(check_maps PRIVATE ddnet_physics)

# The tests need a map of Unique Race. Point DDNET_PHYSICS_UNIQUE_TEST_MAP at
# one, by default run_grass_short of a checkout of
# https://github.com/unique-clan/unique-maps in unique-maps/.
set(DDNET_PHYSICS_UNIQUE_TEST_MAP "${PROJECT_SOURCE_DIR}/unique-maps/maps/run_grass_short.map" CACHE FILEPATH
    "Unique Race map the tests run on")
set(DDNET_PHYSICS_UNIQUE_FASTCAP_TEST_MAP "${PROJECT_SOURCE_DIR}/unique-maps/maps/ctf1.map" CACHE FILEPATH
    "Unique Race fastcap map the tests run on")
add_executable(test_world_copy test_world_copy.c)
target_link_libraries(test_world_copy PRIVATE ddnet_physics)
if(EXISTS "${DDNET_PHYSICS_UNIQUE_FASTCAP_TEST_MAP}")
  add_test(NAME world_copy_fastcap COMMAND test_world_copy "${DDNET_PHYSICS_UNIQUE_FASTCAP_TEST_MAP}" fastcap)
endif()

if(EXISTS "${DDNET_PHYSICS_UNIQUE_TEST_MAP}")
  add_test(NAME world_copy_race COMMAND test_world_copy "${DDNET_PHYSICS_UNIQUE_TEST_MAP}" race)
  # Bit-identity against the real server, if the oracle was built
  # (oracle/build.sh).
  if(EXISTS "${PROJECT_SOURCE_DIR}/build/unique_oracle/build/DDNet-Server")
    add_test(NAME unique_oracle
      COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/oracle/run_suite.sh" "${DDNET_PHYSICS_UNIQUE_TEST_MAP}")
    set_tests_properties(unique_oracle PROPERTIES
      ENVIRONMENT "DDNET_PHYSICS_BUILD=${PROJECT_BINARY_DIR}" TIMEOUT 1800)
  endif()
else()
  message(STATUS "ddnet_physics: no Unique Race test map at ${DDNET_PHYSICS_UNIQUE_TEST_MAP}, tests are not registered")
endif()

# Ticks and world copies per second with random inputs.
add_executable(bench bench.c)
target_link_libraries(bench PRIVATE ddnet_physics)
