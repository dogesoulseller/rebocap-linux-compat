cmake_minimum_required(VERSION 3.16)
project(driver_rebocap CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_path(OPENVR_INCLUDE_DIR openvr_driver.h PATH_SUFFIXES openvr REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

add_library(driver_rebocap SHARED src/driver.cpp)
target_include_directories(driver_rebocap SYSTEM PRIVATE ${OPENVR_INCLUDE_DIR})
target_link_libraries(driver_rebocap PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(driver_rebocap PRIVATE -Wall -Wextra -fvisibility=hidden)
# vrserver runs inside Steam's runtime container. Its libstdc++ is older than the host's.
target_link_options(driver_rebocap PRIVATE -static-libstdc++ -static-libgcc -Wl,--no-undefined
  -Wl,--exclude-libs,ALL -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
set_target_properties(driver_rebocap PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/exports.map)
set_target_properties(driver_rebocap PROPERTIES PREFIX "")
