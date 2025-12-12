add_executable(basic_example basic_example.cpp)
target_link_libraries(basic_example PRIVATE hft-core)


add_executable(udp_sender udp_sender.cpp)
target_link_libraries(udp_sender PRIVATE hft-core)


add_executable(itch50_example itch50_example.cpp)
target_link_libraries(itch50_example PRIVATE hft-core)


add_executable(itch_generator itch_generator.cpp)
target_link_libraries(itch_generator PRIVATE hft-core)
