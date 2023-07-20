cmake_minimum_required(VERSION 3.22)
project(wayland-client LANGUAGES CXX)

find_package(sio REQUIRED)
find_package(fmt REQUIRED)

add_library(wayland
  source/logging.cpp
  source/protocol.cpp)
  # source/renderer.cpp)
target_link_libraries(wayland PUBLIC sio::sio fmt::fmt)

# add_executable(wayland-client
#   source/main.cpp)
# target_link_libraries(wayland-client PRIVATE wayland)

# if (BUILD_TESTING)
  add_subdirectory(tests)
# endif()