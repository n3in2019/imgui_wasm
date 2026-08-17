cmake_minimum_required(VERSION 3.10)
project(imgui_wasm VERSION 0.1.1 LANGUAGES CXX)

# Pure-C++ core (call-stream transport) for ImGuiWasm.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
# Overridable so package managers can pre-seed third_party/imgui with the
# pinned revision; the fetch below then stays a no-op.
set(IMGUI_DIR "${CORE_DIR}/third_party/imgui" CACHE PATH
    "Dear ImGui source directory (the pinned revision is fetched here when missing)")
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

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_program(IMGUI_WASM_PYTHON NAMES python3 python)
if(NOT IMGUI_WASM_PYTHON)
    message(FATAL_ERROR
        "python3 not found: it is required to embed the frontend assets.")
endif()

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
    COMMAND "${IMGUI_WASM_PYTHON}" "${CORE_DIR}/tools/embed_assets.py"
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

add_library(imgui_wasm_core_cpp
    src/net.cpp
    src/capture.cpp
    src/callstream_protocol.cpp
    src/state.cpp
    src/server.cpp
    src/lib.cpp
    src/pam_auth.cpp
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

add_library(imgui_wasm::core ALIAS imgui_wasm_core_cpp)
set_target_properties(imgui_wasm_core_cpp PROPERTIES EXPORT_NAME core)

target_include_directories(imgui_wasm_core_cpp
    PUBLIC
        $<BUILD_INTERFACE:${CORE_DIR}/src>
        $<BUILD_INTERFACE:${CORE_DIR}/include>
        $<BUILD_INTERFACE:${IMGUI_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# The wrapper's imgui_wasm.hpp uses IMGUI_USER_CONFIG="imconfig.h".
target_compile_definitions(imgui_wasm_core_cpp PUBLIC IMGUI_USER_CONFIG="imconfig.h")

target_link_libraries(imgui_wasm_core_cpp PUBLIC Threads::Threads)
# PAM is loaded at runtime (dlopen); only libdl is needed at link time.
target_link_libraries(imgui_wasm_core_cpp PRIVATE ${CMAKE_DL_LIBS})

option(IMGUI_WASM_BUILD_EXAMPLES "Build the C++ core examples" OFF)
option(IMGUI_WASM_BUILD_TESTS "Build the C++ core tests" OFF)

if(IMGUI_WASM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(IMGUI_WASM_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# --- Install ---------------------------------------------------------------------
# Installs the library, public headers, and a CMake package so consumers can
# find_package(imgui_wasm CONFIG REQUIRED) and link imgui_wasm::core. The
# installed package embeds Dear ImGui at the pinned revision: imgui.h and
# siblings ship next to the imgui_wasm headers, minus upstream imconfig.h
# (the repo's include/imconfig.h is the effective user config via the
# IMGUI_USER_CONFIG define).

file(GLOB IMGUI_WASM_IMGUI_PUBLIC_HEADERS "${IMGUI_DIR}/*.h")
list(REMOVE_ITEM IMGUI_WASM_IMGUI_PUBLIC_HEADERS "${IMGUI_DIR}/imconfig.h")

install(TARGETS imgui_wasm_core_cpp
    EXPORT imgui_wasm-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES ${IMGUI_WASM_IMGUI_PUBLIC_HEADERS}
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES LICENSE
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/imgui_wasm")

set(IMGUI_WASM_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_DATAROOTDIR}/imgui_wasm")

install(EXPORT imgui_wasm-targets
    NAMESPACE imgui_wasm::
    DESTINATION "${IMGUI_WASM_CMAKE_INSTALL_DIR}")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/imgui_wasmConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/imgui_wasmConfig.cmake"
    INSTALL_DESTINATION "${IMGUI_WASM_CMAKE_INSTALL_DIR}")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/imgui_wasmConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/imgui_wasmConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/imgui_wasmConfigVersion.cmake"
    DESTINATION "${IMGUI_WASM_CMAKE_INSTALL_DIR}")
