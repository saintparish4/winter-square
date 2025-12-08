add_executable(test_lockfree_queue test_lockfree_queue.cpp)
target_link_libraries(test_lockfree_queue PRIVATE hft-core)
add_test(NAME lockfree_queue COMMAND test_lockfree_queue)

# Add more tests here as they are created

