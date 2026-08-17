if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    # The library embeds its own Dear ImGui copy at a pinned revision; a
    # shared build would duplicate ImGui symbols across DSOs. Static only.
    message(STATUS "imgui-wasm: forcing static library linkage")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()

# Pre-release validation: set IMGUI_WASM_LOCAL_SOURCE_DIR to a working tree
# (e.g. this repository) to build the port from it instead of the release
# tag. The release path below fetches from GitHub with pinned hashes; strip
# this branch when submitting the port to microsoft/vcpkg.
if(DEFINED ENV{IMGUI_WASM_LOCAL_SOURCE_DIR})
    set(IMGUI_WASM_LOCAL "$ENV{IMGUI_WASM_LOCAL_SOURCE_DIR}")
    set(SOURCE_PATH "${CURRENT_BUILDTREES_DIR}/src/local")
    file(REMOVE_RECURSE "${SOURCE_PATH}")
    file(COPY
        "${IMGUI_WASM_LOCAL}/CMakeLists.txt"
        "${IMGUI_WASM_LOCAL}/LICENSE"
        "${IMGUI_WASM_LOCAL}/cmake"
        "${IMGUI_WASM_LOCAL}/include"
        "${IMGUI_WASM_LOCAL}/src"
        "${IMGUI_WASM_LOCAL}/frontend"
        "${IMGUI_WASM_LOCAL}/wasm"
        DESTINATION "${SOURCE_PATH}")
    file(COPY "${IMGUI_WASM_LOCAL}/tools/embed_assets.py"
        DESTINATION "${SOURCE_PATH}/tools")
else()
    vcpkg_from_github(
        OUT_SOURCE_PATH SOURCE_PATH
        REPO n3in2019/imgui_wasm
        REF "v${VERSION}"
        SHA512 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000
    )
endif()

# Dear ImGui is vendored at the exact commit pinned by upstream's CMake
# (v1.92.8, docking branch): the call-stream protocol, generated capture
# bindings, and the browser WASM twin are revision-coupled, so the port
# cannot track the standalone imgui port. Pre-seeding third_party/imgui
# keeps the configure-time fetch a no-op.
vcpkg_from_git(
    OUT_SOURCE_PATH IMGUI_SOURCE_PATH
    URL "https://github.com/ocornut/imgui.git"
    REF "b61e56346a92cfcaf1f43a545ca37b0b32239654"
)
file(COPY "${IMGUI_SOURCE_PATH}/" DESTINATION "${SOURCE_PATH}/third_party/imgui")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DIMGUI_WASM_BUILD_EXAMPLES=OFF
        -DIMGUI_WASM_BUILD_TESTS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME imgui_wasm)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(
    COMMENT "ImGuiWasm is MIT licensed. It embeds Dear ImGui (MIT) and generated dear_bindings outputs (MIT)."
    FILE_LIST
        "${SOURCE_PATH}/LICENSE"
        "${IMGUI_SOURCE_PATH}/LICENSE.txt"
)
