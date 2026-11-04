cmake_minimum_required(VERSION 3.24)
project(old-town-walk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# A release needs no Visual C++ runtime installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Source paths that reach the program through __FILE__ stay relative, so a build names no folder on the machine.
if(MSVC)
  file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" _source_dir)
  add_compile_options("/d1trimfile:${_source_dir}")
else()
  add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)
endif()

# Strata, pinned to a full commit SHA (see strata.ref). Not cached, so a new ref reaches an existing build.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/strata.ref" _strata_ref)
string(STRIP "${_strata_ref}" _strata_ref)
option(OLD_TOWN_WALK_WITH_WINDOW "Build Strata's SDL3 window surface (interactive viewer)" ON)

include(FetchContent)
set(STRATA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STRATA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(STRATA_WITH_USD OFF CACHE BOOL "" FORCE)
set(STRATA_WITH_SDL ${OLD_TOWN_WALK_WITH_WINDOW} CACHE BOOL "" FORCE)
FetchContent_Declare(strata
  GIT_REPOSITORY https://github.com/Preponderous-Software/strata.git
  GIT_TAG ${_strata_ref})
FetchContent_MakeAvailable(strata)

add_library(old_town_walk_tiles STATIC app/tile.cpp)
target_include_directories(old_town_walk_tiles PUBLIC app)

add_executable(old-town-walk app/main.cpp)
target_link_libraries(old-town-walk PRIVATE old_town_walk_tiles strata::static)
if(NOT MSVC)
  target_compile_options(old_town_walk_tiles PRIVATE -Wall -Wextra)
  target_compile_options(old-town-walk PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_executable(test_app tests/cpp/test_app.cpp)
target_link_libraries(test_app PRIVATE old_town_walk_tiles)
add_test(NAME app_units COMMAND test_app ${CMAKE_CURRENT_SOURCE_DIR}/build/tiles)
# Headless render (needs build/tiles from `uv run old-town-walk build`), passing through enough tiles to unload some.
add_test(NAME render_fly
  COMMAND $<TARGET_FILE:old-town-walk> --headless --width 640 --height 360 --fly --altitude 120 --yaw 90 --pitch -35 --walk-ahead 3
          --tiles ${CMAKE_CURRENT_SOURCE_DIR}/build/tiles --screenshot ${CMAKE_CURRENT_BINARY_DIR}/fly.png)
add_test(NAME render_walk
  COMMAND $<TARGET_FILE:old-town-walk> --headless --width 640 --height 360 --yaw 90 --radius 300 --pass-east -900
          --tiles ${CMAKE_CURRENT_SOURCE_DIR}/build/tiles --screenshot ${CMAKE_CURRENT_BINARY_DIR}/walk.png)
set_tests_properties(render_walk render_fly PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1;GALLIUM_DRIVER=llvmpipe")
