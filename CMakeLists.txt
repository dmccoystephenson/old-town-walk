cmake_minimum_required(VERSION 3.24)
project(old-town-walk VERSION 0.3.0 LANGUAGES C CXX)

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

# The browser build: Emscripten, Strata's WebGL 2 backend and its web kit, which owns the context, so no SDL.
if(EMSCRIPTEN)
  set(OLD_TOWN_WALK_WITH_WINDOW OFF CACHE BOOL "" FORCE)
  add_compile_options(-fwasm-exceptions)
  add_link_options(-fwasm-exceptions -sALLOW_MEMORY_GROWTH=1)
  set(STRATA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(STRATA_WITH_SIM OFF CACHE BOOL "" FORCE)
endif()

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

if(EMSCRIPTEN)
  add_executable(old-town-walk-web app/web_main.cpp)
  target_link_libraries(old-town-walk-web PRIVATE old_town_walk_tiles strata::static)
  target_include_directories(old-town-walk-web PRIVATE "${strata_SOURCE_DIR}/web/kit")
  target_compile_options(old-town-walk-web PRIVATE -Wall -Wextra -Wno-unused-function)
  set_target_properties(old-town-walk-web PROPERTIES OUTPUT_NAME walk SUFFIX ".js")
  # The kit's exports (Strata's web/kit/build-kit.sh), plus this page's. wasmBinary is in the incoming
  # list so that the bytes the loading shell fetched are not downloaded a second time.
  set(_incoming "ENVIRONMENT,arguments,canvas,dynamicLibraries,elementPointerLock,instantiateWasm,locateFile")
  string(APPEND _incoming ",monitorRunDependencies,noExitRuntime,noInitialRun,onAbort,onExit,onRuntimeInitialized,postRun")
  string(APPEND _incoming ",preInit,preRun,print,printErr,setStatus,statusMessage,stderr,stdin,stdout,thisProgram,wasm")
  string(APPEND _incoming ",websocket,wasmBinary")
  set(_exports "_malloc,_free,_sk_init,_sk_restore,_sk_kit_version,_sk_resize,_sk_set_quality,_sk_set_camera,_sk_set_sun")
  string(APPEND _exports ",_sk_view_proj,_sk_stats,_sk_frame")
  string(APPEND _exports ",_otw_start,_otw_tile,_otw_step,_otw_toggle_fly,_otw_set_radius,_otw_status,_otw_jump,_otw_set_speeds,_otw_limit,_otw_go_to_start,_otw_toggle_bike,_otw_set_bike_speed,_otw_set_fov")
  target_link_options(old-town-walk-web PRIVATE
    -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sENVIRONMENT=web -sMODULARIZE=1 -sEXPORT_NAME=createWalk
    -sSTACK_SIZE=1MB -sINITIAL_MEMORY=96MB
    "-sEXPORTED_FUNCTIONS=${_exports}" "-sINCOMING_MODULE_JS_API=${_incoming}"
    -sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAP32,HEAPU8,stringToNewUTF8,UTF8ToString)
else()
  add_executable(old-town-walk app/main.cpp)
  target_link_libraries(old-town-walk PRIVATE old_town_walk_tiles strata::static)
endif()
if(NOT MSVC)
  target_compile_options(old_town_walk_tiles PRIVATE -Wall -Wextra)
endif()
if(NOT MSVC AND NOT EMSCRIPTEN)
  target_compile_options(old-town-walk PRIVATE -Wall -Wextra)
endif()
if(EMSCRIPTEN)
  return()  # The tests below run natively; web/smoke tests the page.
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
