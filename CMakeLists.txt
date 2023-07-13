cmake_minimum_required(VERSION 3.22)
project(wayland-client LANGUAGES CXX)

find_package(sio REQUIRED)
find_package(fmt REQUIRED)

add_executable(wayland-client
  source/connection.cpp
  source/main.cpp
  source/logging.cpp
  source/protocol.cpp
  source/renderer.cpp)
target_link_libraries(wayland-client PRIVATE sio::sio fmt::fmt)