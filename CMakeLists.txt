cmake_minimum_required(VERSION 3.15)
project(ddnet_physics C)

option(DDNET_PHYSICS_BUILD_TESTS "Build the tests and the oracle comparison tool" ON)
option(DDNET_PHYSICS_BUILD_EXAMPLES "Build the examples" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(libs/ddnet_maploader EXCLUDE_FROM_ALL)

# Bit-identical results depend on the compiler doing the float math exactly as
# written: no fused multiply-add, no reordering, no x87 excess precision.
add_library(ddnet_physics_options INTERFACE)
target_compile_options(ddnet_physics_options INTERFACE
  -ffp-contract=off -fno-fast-math -fno-math-errno
  -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(ddnet_physics_options INTERFACE _DEFAULT_SOURCE)

# The two implementations have different structures, so one library is one of
# them: the backend is chosen here, at compile time.
set(DDNET_PHYSICS_BACKEND reference CACHE STRING
    "Implementation to build: reference (readable port of DDNet) or optimized")
set_property(CACHE DDNET_PHYSICS_BACKEND PROPERTY STRINGS reference optimized)

set(DDNET_PHYSICS_COMMON
  src/common/config.c
  src/common/console.c
  src/common/tuning.c)

if(DDNET_PHYSICS_BACKEND STREQUAL "reference")
  set(DDNET_PHYSICS_BACKEND_SOURCES
    src/reference/character.c
    src/reference/character_core.c
    src/reference/collision.c
    src/reference/entities.c
    src/reference/gamecontroller.c
    src/reference/teams.c
    src/reference/world.c)
  set(DDNET_PHYSICS_BACKEND_DEFINITION DDNET_PHYSICS_BACKEND_REFERENCE)
elseif(DDNET_PHYSICS_BACKEND STREQUAL "optimized")
  # One translation unit, so that everything on the tick path can be inlined.
  set(DDNET_PHYSICS_BACKEND_SOURCES src/optimized/unity.c)
  # The event build (unity_events.c): the same code once more, which calls the
  # callbacks of include/ddnet_physics/events.h, as the functions ddnet_ev_*.
  # The build without events is not changed by it.
  option(DDNET_PHYSICS_EVENTS "Also build the optimized backend with events (ddnet_ev_*)" ON)
  if(DDNET_PHYSICS_EVENTS)
    list(APPEND DDNET_PHYSICS_BACKEND_SOURCES src/optimized/unity_events.c)
    set(DDNET_PHYSICS_EVENTS_DEFINITION DDNET_PHYSICS_HAS_EVENTS)
  endif()
  # GCC stops inlining earlier than is good for it here: 3 % more ticks per
  # second on every map that was tried. (What must stay out of line is marked
  # DDNET_NOINLINE.)
  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(src/optimized/unity.c src/optimized/unity_events.c PROPERTIES COMPILE_OPTIONS
      "-finline-limit=2000;--param=max-inline-insns-single=2000;--param=inline-unit-growth=400")
  endif()
  set(DDNET_PHYSICS_BACKEND_DEFINITION DDNET_PHYSICS_BACKEND_OPTIMIZED)
else()
  message(FATAL_ERROR "ddnet_physics: unknown DDNET_PHYSICS_BACKEND '${DDNET_PHYSICS_BACKEND}'")
endif()

# A shared library for programs that load several modules which all simulate
# worlds of one another: one copy of the library for all of them (the worlds
# are numbered by it, see ddnet_world_copy). Only the functions ddnet_* are
# exported.
option(DDNET_PHYSICS_SHARED "Build a shared library" OFF)
if(DDNET_PHYSICS_SHARED)
  add_library(ddnet_physics SHARED ${DDNET_PHYSICS_COMMON} ${DDNET_PHYSICS_BACKEND_SOURCES})
  set_target_properties(ddnet_physics PROPERTIES POSITION_INDEPENDENT_CODE ON)
  set_target_properties(ddnet_map_loader PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(NOT WIN32 AND NOT APPLE)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ddnet_physics.map" "{ global: ddnet_*; local: *; };\n")
    target_link_options(ddnet_physics PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/ddnet_physics.map")
  endif()
else()
  add_library(ddnet_physics STATIC ${DDNET_PHYSICS_COMMON} ${DDNET_PHYSICS_BACKEND_SOURCES})
endif()

# The instructions of the CPU this is built on (AVX and so on). The results
# stay the same: nothing is contracted or reordered (see above), the same
# operations only take fewer instructions. Worth 3 % for the optimized backend.
# The library then only runs on CPUs that have what this one has.
option(DDNET_PHYSICS_NATIVE "Build for the CPU of this machine (-march=native)" OFF)
# Or for a set of CPUs, for a library that is handed out: x86-64-v3 (AVX2 and
# FMA, Haswell and later) takes the paths for SSE4.1 and AVX the code has and
# gives the same results (the FMA build of glibc's powf is one of the two that
# velocity_ramp_prepare() knows).
set(DDNET_PHYSICS_ARCH "" CACHE STRING "-march for the library if not native (for example x86-64-v3); empty for the compiler's default")
if(DDNET_PHYSICS_NATIVE)
  target_compile_options(ddnet_physics PRIVATE -march=native)
elseif(DDNET_PHYSICS_ARCH)
  target_compile_options(ddnet_physics PRIVATE -march=${DDNET_PHYSICS_ARCH})
endif()

# Jumps kept off 32-byte boundaries. On Intel CPUs from Skylake to Comet Lake
# with the microcode for their JCC erratum, a jump that crosses or ends on one
# makes its 32 bytes of code come from the decoders on every pass instead of
# from the cache of decoded instructions, which the tick of a tee does not fit
# into anyway. The assembler pads the code with prefixes and no-ops; nothing
# else changes. On a Core i5-7500 (Kaby Lake) 0.4 % faster without a profile,
# and 2 % with random inputs and 0.9 % on the run with one.
option(DDNET_PHYSICS_JCC_PADDING "Keep jumps off 32-byte boundaries (Intel's JCC erratum), if the toolchain can"
       ON)
if(DDNET_PHYSICS_JCC_PADDING AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|i.86")
  include(CheckCCompilerFlag)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(DDNET_PHYSICS_JCC_FLAG -mbranches-within-32B-boundaries)
  else()
    set(DDNET_PHYSICS_JCC_FLAG -Wa,-mbranches-within-32B-boundaries)
  endif()
  check_c_compiler_flag(${DDNET_PHYSICS_JCC_FLAG} DDNET_PHYSICS_HAVE_JCC_FLAG)
  if(DDNET_PHYSICS_HAVE_JCC_FLAG)
    target_compile_options(ddnet_physics PRIVATE ${DDNET_PHYSICS_JCC_FLAG})
  endif()
endif()
target_include_directories(ddnet_physics PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(ddnet_physics PUBLIC
  ${DDNET_PHYSICS_BACKEND_DEFINITION} ${DDNET_PHYSICS_EVENTS_DEFINITION})
target_link_libraries(ddnet_physics PUBLIC ddnet_map_loader PRIVATE ddnet_physics_options m)
set_target_properties(ddnet_physics PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# Profile guided optimization. It changes how the code is laid out and what is
# inlined, not what is computed: the results stay the same. With GCC and the
# profile of tests/pgo.sh (the run of bench --tas and random inputs on the maps
# given) the optimized backend is about 4 % faster on the run and 9 % with
# random inputs; Clang, with a profile or without, was slower than GCC.
set(DDNET_PHYSICS_PGO off CACHE STRING
    "off, generate (a build that records a profile when it runs) or use (optimize with a recorded profile)")
set_property(CACHE DDNET_PHYSICS_PGO PROPERTY STRINGS off generate use)
set(DDNET_PHYSICS_PGO_PROFILE "${CMAKE_BINARY_DIR}/ddnet_physics.profdata" CACHE PATH
    "The profile: a file merged by llvm-profdata for Clang, a directory for GCC")
if(DDNET_PHYSICS_PGO STREQUAL "generate")
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(ddnet_physics PRIVATE -fprofile-instr-generate)
    target_link_options(ddnet_physics PUBLIC -fprofile-instr-generate)
  else()
    target_compile_options(ddnet_physics PRIVATE -fprofile-generate=${DDNET_PHYSICS_PGO_PROFILE})
    target_link_options(ddnet_physics PUBLIC -fprofile-generate=${DDNET_PHYSICS_PGO_PROFILE})
  endif()
elseif(DDNET_PHYSICS_PGO STREQUAL "use")
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(ddnet_physics PRIVATE -fprofile-instr-use=${DDNET_PHYSICS_PGO_PROFILE})
  else()
    target_compile_options(ddnet_physics PRIVATE
      -fprofile-use=${DDNET_PHYSICS_PGO_PROFILE} -fprofile-correction -Wno-missing-profile)
  endif()
elseif(NOT DDNET_PHYSICS_PGO STREQUAL "off")
  message(FATAL_ERROR "ddnet_physics: unknown DDNET_PHYSICS_PGO '${DDNET_PHYSICS_PGO}'")
endif()

if(DDNET_PHYSICS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DDNET_PHYSICS_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
