add_executable(test_connection test_connection.cpp)
target_link_libraries(test_connection PRIVATE wayland)

add_executable(test_buffer test_buffer.cpp)
target_link_libraries(test_buffer PRIVATE wayland)

add_executable(test_font test_font.cpp)
target_link_libraries(test_font PRIVATE wayland)

add_executable(test_renderer test_renderer.cpp)
target_link_libraries(test_renderer PRIVATE wayland)

add_executable(test_window test_window.cpp)
target_link_libraries(test_window PRIVATE wayland)