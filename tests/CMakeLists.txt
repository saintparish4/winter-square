add_executable(test_lockfree_queue test_lockfree_queue.cpp)
target_link_libraries(test_lockfree_queue PRIVATE hft-core)
add_test(NAME lockfree_queue COMMAND test_lockfree_queue)

add_executable(test_itch50_parser test_itch50_parser.cpp)
target_link_libraries(test_itch50_parser PRIVATE hft-core)
add_test(NAME itch50_parser COMMAND test_itch50_parser)
