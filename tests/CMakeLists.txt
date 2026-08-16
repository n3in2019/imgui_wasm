add_executable(core_cpp_tests protocol_tests.cpp)
target_link_libraries(core_cpp_tests PRIVATE imgui_ws_core_cpp)
enable_testing()
add_test(NAME protocol COMMAND core_cpp_tests)
