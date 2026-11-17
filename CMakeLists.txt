cmake_minimum_required(VERSION 3.24)
project(iw4radiant LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG 1ab953d75a760d5102eb2d5c4c6c4e65df6b29f3) # docking branch
# SOURCE_SUBDIR points nowhere so miniz's own CMake project is not configured.
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.1.2
  SOURCE_SUBDIR _unused)
FetchContent_MakeAvailable(imgui miniz)

# miniz.h includes a header its CMake project normally generates.
file(WRITE ${CMAKE_BINARY_DIR}/miniz_gen/miniz_export.h "#pragma once\n#define MINIZ_EXPORT\n")
add_library(miniz_lib STATIC
  ${miniz_SOURCE_DIR}/miniz.c
  ${miniz_SOURCE_DIR}/miniz_tdef.c
  ${miniz_SOURCE_DIR}/miniz_tinfl.c
  ${miniz_SOURCE_DIR}/miniz_zip.c)
target_include_directories(miniz_lib PUBLIC ${miniz_SOURCE_DIR} ${CMAKE_BINARY_DIR}/miniz_gen)

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

# Core has no D3D or UI dependency, so the tests can link it directly.
add_library(iw4r_core STATIC
  src/brush.cpp
  src/brushshapes.cpp
  src/patch.cpp
  src/worldgeom.cpp
  src/assetmanager.cpp
  src/terrain.cpp
  src/fxpreview.cpp
  src/document.cpp
  src/mapio.cpp
  src/iwi.cpp
  src/fastfile.cpp
  src/zone/zonewriter.cpp
  src/zone/clipmap.cpp
  src/zone/gfxworld.cpp
  src/zone/zonesim.cpp
  src/zone/zonedump.cpp
  src/zone/mapimport.cpp
  src/zone/zonebuild.cpp
  src/light/lightbake.cpp
  src/light/colorgrade.cpp
  src/light/gpubake.cpp
  src/zone/skysource.cpp
  src/zone/matdefs.cpp
  src/zone/modelbuild.cpp
  src/zone/animbuild.cpp
  src/zone/weapondef.cpp
  src/zone/pathnodes.cpp
  src/zone/efx.cpp
  src/zone/bo1fx.cpp
  src/model/xanimexport.cpp
  src/model/imageio.cpp
  src/model/xmodelexport.cpp
  src/model/skeleton.cpp
  src/live/process.cpp
  src/live/liveimport.cpp)
target_include_directories(iw4r_core PUBLIC src)
target_link_libraries(iw4r_core PUBLIC miniz_lib d3d11 dxgi d3dcompiler)

add_executable(iw4radiant WIN32
  src/main.cpp
  src/app.cpp
  src/uitest.cpp)
target_link_libraries(iw4radiant PRIVATE iw4r_ui)

# UI shared by Mod Tools and Radiant: renderer, material/texture database, the Asset Manager window.
add_library(iw4r_ui STATIC
  src/assetpanel.cpp
  src/modelpreview.cpp
  src/assets.cpp
  src/renderer.cpp)
target_link_libraries(iw4r_ui PUBLIC iw4r_core imgui_lib d3d11 dxgi d3dcompiler comdlg32 shell32 ole32)

# IW4 Mod Tools: the main program (maps, Asset Manager), launches Radiant.
add_executable(iw4modtools WIN32
  src/modtools/main.cpp
  src/modtools/modtools.cpp)
target_link_libraries(iw4modtools PRIVATE iw4r_ui)
add_dependencies(iw4modtools iw4radiant)  # launched from Mod Tools, so built with it

add_executable(iw4r_zone src/zonetool_main.cpp)
target_link_libraries(iw4r_zone PRIVATE iw4r_core)

# Injectable loader so the editor's compiled maps can be devmap'd (redirects *.ff opens to the
# deploy folder). x64 only; built next to the editor so Compile & Run can inject it.
add_library(iw4r_mapload SHARED src/dll/mapload_dll.cpp src/dll/volumetric.cpp)
target_include_directories(iw4r_mapload PRIVATE src)
target_link_libraries(iw4r_mapload PRIVATE psapi d3d9 dxguid)
if(MSVC)
  target_compile_definitions(iw4r_mapload PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

add_executable(iw4r_tests tests/tests.cpp)
target_link_libraries(iw4r_tests PRIVATE iw4r_core)
enable_testing()
add_test(NAME core COMMAND iw4r_tests)

if(MSVC)
  target_compile_options(iw4r_core PRIVATE /W4 /utf-8)
  target_compile_options(iw4radiant PRIVATE /W4 /utf-8)
  target_compile_options(iw4r_ui PRIVATE /W4 /utf-8)
  target_compile_options(iw4modtools PRIVATE /W4 /utf-8)
  target_compile_options(iw4r_tests PRIVATE /W4 /utf-8)
  target_compile_definitions(iw4r_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

# Compile-time guard for the reverse-engineered x64 asset layouts.
add_executable(iw4r_structcheck tools/structcheck.cpp)
target_include_directories(iw4r_structcheck PRIVATE src)
if(MSVC)
  target_compile_options(iw4r_structcheck PRIVATE /W0 /wd4324)
endif()
add_test(NAME structcheck COMMAND iw4r_structcheck)

# Offline check of the loader DLL's volumetric pass (fake game: redirected zone, floatz, camera).
add_executable(iw4r_voltest tests/voltest.cpp)
target_link_libraries(iw4r_voltest PRIVATE d3d9)
