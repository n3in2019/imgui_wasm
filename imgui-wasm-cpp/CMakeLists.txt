cmake_minimum_required(VERSION 3.10)
project(imgui-wasm-cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui-wasm-core/third_party/imgui")

set(IMGUI_WASM_CORE_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../target/debug/libimgui_wasm.a")
if(NOT EXISTS "${IMGUI_WASM_CORE_LIB}")
    set(IMGUI_WASM_CORE_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../target/release/libimgui_wasm.a")
endif()

add_library(imgui-wasm-cpp STATIC
    src/imgui_wasm.cpp
)

target_include_directories(imgui-wasm-cpp
    PUBLIC include ${IMGUI_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../imgui-wasm-core/include
)

target_compile_definitions(imgui-wasm-cpp PUBLIC IMGUI_USER_CONFIG="imconfig.h")

target_link_libraries(imgui-wasm-cpp PUBLIC ${IMGUI_WASM_CORE_LIB} stdc++ m)

if(APPLE)
    target_link_libraries(imgui-wasm-cpp PUBLIC "-framework CoreFoundation")
endif()

option(IMGUI_WASM_BUILD_EXAMPLES "Build ImGuiWasm examples" OFF)

if(IMGUI_WASM_BUILD_EXAMPLES)
    add_executable(example_cpp examples/example.cpp)
    target_include_directories(example_cpp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../imgui-wasm-core/include)
    target_link_libraries(example_cpp imgui-wasm-cpp)

    # Call-stream variant of the example: builds the same source with
    # -DIMGUI_WASM_EXAMPLE_CALLSTREAM enables call-stream transport while the
    # source continues to use the ordinary ImGui:: API. Cargo always builds the
    # required WASM twin.
    add_executable(example_cpp_callstream examples/example.cpp)
    target_include_directories(example_cpp_callstream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../imgui-wasm-core/include)
    target_compile_definitions(example_cpp_callstream PRIVATE IMGUI_WASM_EXAMPLE_CALLSTREAM)
    target_link_libraries(example_cpp_callstream imgui-wasm-cpp)
endif()
