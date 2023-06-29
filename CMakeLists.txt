cmake_minimum_required(VERSION 3.22)
project(ChatServer LANGUAGES CXX)

find_package(sio REQUIRED)
find_package(fmt REQUIRED)

add_executable(ChatServer
  source/connection.cpp
  source/main.cpp
  source/logging.cpp)
target_link_libraries(ChatServer PRIVATE sio::sio fmt::fmt)