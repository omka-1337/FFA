cmake_minimum_required(VERSION 3.20)
project(FFAEngine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(ffa_script
  src/audio/Bink.cpp
  src/audio/SoundBank.cpp
  src/audio/LipSync.cpp
  src/core/Library.cpp
  src/core/Name.cpp
  src/core/Package.cpp
  src/render/Material.cpp
  src/render/Texture.cpp
  src/script/Bytecode.cpp
  src/script/Tagged.cpp
  src/script/Types.cpp
  src/script/Linker.cpp
  src/script/VM.cpp
  src/script/NativesCore.cpp
  src/world/Animation.cpp
  src/world/Animator.cpp
  src/world/Bsp.cpp
  src/world/Collision.cpp
  src/world/Level.cpp
  src/world/AI.cpp
  src/world/Karma.cpp
  src/world/Movie.cpp
  src/world/Gui.cpp
  src/world/Audio.cpp
  src/world/Physics.cpp
  src/world/Session.cpp
  src/world/SkeletalMesh.cpp
  src/world/StaticMesh.cpp
  src/world/Sweep.cpp
  src/world/Terrain.cpp
  src/world/World.cpp
)
target_include_directories(ffa_script PUBLIC src)

add_executable(ffa-script apps/ffa_script.cpp)
target_link_libraries(ffa-script PRIVATE ffa_script)

# The game's window: SDL2 for the window and input, OpenGL ES 2 to draw, what
# every PortMaster handheld has. Built only when both are found.
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(SDL2 IMPORTED_TARGET sdl2)
  pkg_check_modules(GLESV2 IMPORTED_TARGET glesv2)
endif()
if(SDL2_FOUND AND GLESV2_FOUND)
  add_library(ffa_render src/render/LevelRender.cpp src/render/Hud.cpp src/render/Font.cpp)
  target_link_libraries(ffa_render PUBLIC ffa_script PkgConfig::GLESV2)
  find_package(Threads REQUIRED)
  add_executable(ffa-play apps/ffa_play.cpp)
  target_link_libraries(ffa-play PRIVATE ffa_render PkgConfig::SDL2 Threads::Threads)
else()
  message(STATUS "SDL2 or OpenGL ES 2 not found: ffa-play is not built")
endif()

# The tests run against packages written by tests/fixtures.py, which checks them
# with the Python readers in tools/ first, so testing needs Python 3.
set(FFA_TOOLS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools CACHE PATH "The Python tools directory")
enable_testing()
find_package(Python3 COMPONENTS Interpreter)
if(NOT EXISTS ${FFA_TOOLS_DIR}/uscript.py)
  message(STATUS "The Python tools are not at ${FFA_TOOLS_DIR}: tests are not built")
elseif(Python3_FOUND)
  set(FIXTURES ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
  add_custom_command(
    OUTPUT ${FIXTURES}/Core.u ${FIXTURES}/Game.u
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures.py ${FIXTURES} ${FFA_TOOLS_DIR}
    DEPENDS tests/fixtures.py
    COMMENT "Writing test packages")
  add_custom_target(fixtures ALL DEPENDS ${FIXTURES}/Core.u ${FIXTURES}/Game.u)
  add_executable(test-vm tests/test_vm.cpp)
  target_link_libraries(test-vm PRIVATE ffa_script)
  add_dependencies(test-vm fixtures)
  add_test(NAME vm COMMAND test-vm ${FIXTURES})
endif()
