cmake_minimum_required(VERSION 3.16)
project(borges_contract CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# lib/BorgesCore is compiled as is: the only thing this target adds is host ports.
set(BORGES_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/BorgesCore)
file(GLOB BORGES_CORE_SOURCES CONFIGURE_DEPENDS ${BORGES_CORE_DIR}/*.cpp)

add_executable(borges-cli borges_cli.cpp ${BORGES_CORE_SOURCES})
target_include_directories(borges-cli PRIVATE ${BORGES_CORE_DIR})
target_compile_options(borges-cli PRIVATE -Wall -Wextra)
target_link_libraries(borges-cli PRIVATE ws2_32 bcrypt shell32)
target_link_options(borges-cli PRIVATE -static)
