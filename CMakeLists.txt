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
  src/core/Name.cpp
  src/core/Package.cpp
  src/script/Bytecode.cpp
  src/script/Tagged.cpp
  src/script/Types.cpp
  src/script/Linker.cpp
  src/script/VM.cpp
  src/script/NativesCore.cpp
  src/world/Level.cpp
  src/world/World.cpp
)
target_include_directories(ffa_script PUBLIC src)

add_executable(ffa-script apps/ffa_script.cpp)
target_link_libraries(ffa-script PRIVATE ffa_script)

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
