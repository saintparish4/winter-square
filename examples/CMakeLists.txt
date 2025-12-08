add_executable(basic_example basic_example.cpp)
target_link_libraries(basic_example PRIVATE hft-core)

add_executable(udp_sender udp_sender.cpp)
target_link_libraries(udp_sender PRIVATE hft-core)

