cmake_minimum_required(VERSION 3.10)
project(imgui_ws LANGUAGES CXX)

# Pure-C++ core (call-stream transport) for ImGuiWasm.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(IMGUI_DIR "${CORE_DIR}/third_party/imgui")
set(IMGUI_REVISION "b61e56346a92cfcaf1f43a545ca37b0b32239654")

# Fetch the pinned Dear ImGui docking revision on first configure. The
# directory is local dependency state and must not be committed.
if(NOT EXISTS "${IMGUI_DIR}/imgui.h")
    find_package(Git REQUIRED)
    message(STATUS "Fetching pinned Dear ImGui (${IMGUI_REVISION})...")
    file(MAKE_DIRECTORY "${CORE_DIR}/third_party")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" init "${IMGUI_DIR}"
        RESULT_VARIABLE imgui_init_result)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${IMGUI_DIR}" fetch --depth 1
                https://github.com/ocornut/imgui.git "${IMGUI_REVISION}"
        RESULT_VARIABLE imgui_fetch_result)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${IMGUI_DIR}" checkout FETCH_HEAD
        RESULT_VARIABLE imgui_checkout_result)
    if(NOT imgui_init_result EQUAL 0 OR NOT imgui_fetch_result EQUAL 0
            OR NOT imgui_checkout_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to fetch Dear ImGui ${IMGUI_REVISION} into ${IMGUI_DIR}. "
            "Check network access and Git availability, then reconfigure.")
    endif()
endif()

# The browser replay twin artifacts are committed. Regenerate them with
# 'python3 tools/build_wasm_twin.py' (requires Emscripten); the C++
# build itself never invokes Emscripten, it only embeds the existing files.
set(REPLAY_JS "${CORE_DIR}/wasm/imgui_wasm_replay.js")
set(REPLAY_WASM "${CORE_DIR}/wasm/imgui_wasm_replay.wasm")
if(NOT EXISTS "${REPLAY_JS}" OR NOT EXISTS "${REPLAY_WASM}")
    message(FATAL_ERROR
        "WASM replay twin not found under ${CORE_DIR}/wasm/. "
        "Run 'python3 tools/build_wasm_twin.py' once to build it "
        "(requires Emscripten).")
endif()

find_package(Threads REQUIRED)

# --- Embedded frontend assets --------------------------------------------------

set(EMBEDDED_ASSETS_CPP "${CMAKE_CURRENT_BINARY_DIR}/embedded_assets.cpp")
set(ASSET_DEPS
    "${CORE_DIR}/frontend/index.html"
    "${CORE_DIR}/frontend/style.css"
    "${CORE_DIR}/frontend/imgui_wasm.js"
    "${CORE_DIR}/frontend/imgui_wasm_callstream.js"
    "${REPLAY_JS}"
    "${REPLAY_WASM}")
add_custom_command(
    OUTPUT "${EMBEDDED_ASSETS_CPP}"
    COMMAND python3 "${CORE_DIR}/tools/embed_assets.py"
            "${EMBEDDED_ASSETS_CPP}"
            "/|html|${CORE_DIR}/frontend/index.html"
            "/style.css|css|${CORE_DIR}/frontend/style.css"
            "/imgui_wasm.js|js|${CORE_DIR}/frontend/imgui_wasm.js"
            "/imgui_wasm_callstream.js|js|${CORE_DIR}/frontend/imgui_wasm_callstream.js"
            "/imgui_wasm_replay.js|js|${REPLAY_JS}"
            "/imgui_wasm_replay.wasm|wasm|${REPLAY_WASM}"
    DEPENDS "${CORE_DIR}/tools/embed_assets.py" ${ASSET_DEPS}
    VERBATIM
    COMMENT "Embedding ImGuiWasm frontend assets")

# --- Core library ---------------------------------------------------------------

add_library(imgui_ws_core_cpp STATIC
    src/net.cpp
    src/capture.cpp
    src/callstream_protocol.cpp
    src/state.cpp
    src/server.cpp
    src/lib.cpp
    # C++ convenience wrapper (Server + on_render).
    src/imgui_wasm.cpp
    "${EMBEDDED_ASSETS_CPP}"
    # Owns the Dear ImGui context; talks to this library through
    # imgui_wasm_core_api_t.
    src/imgui_backend.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp")

target_include_directories(imgui_ws_core_cpp
    PUBLIC src include "${IMGUI_DIR}"
    PRIVATE "${CORE_DIR}/src")

# The wrapper's imgui_wasm.hpp uses IMGUI_USER_CONFIG="imconfig.h".
target_compile_definitions(imgui_ws_core_cpp PUBLIC IMGUI_USER_CONFIG="imconfig.h")

target_link_libraries(imgui_ws_core_cpp PUBLIC Threads::Threads)

option(IMGUI_WS_BUILD_EXAMPLES "Build the C++ core examples" ON)
option(IMGUI_WS_BUILD_TESTS "Build the C++ core tests" ON)

if(IMGUI_WS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(IMGUI_WS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
