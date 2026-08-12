use std::env;
use std::path::Path;
use std::process::Command;

const IMGUI_REVISION: &str = "b61e56346a92cfcaf1f43a545ca37b0b32239654";

fn main() {
    let manifest = env::var("CARGO_MANIFEST_DIR").unwrap();
    let imgui_dir = Path::new(&manifest).join("third_party/imgui");
    println!("cargo:rerun-if-changed=src/imgui_backend.cpp");
    println!("cargo:rerun-if-changed=src/imgui_wasm_imgui.cpp");
    println!("cargo:rerun-if-changed=src/imgui_wasm_internal.h");
    println!("cargo:rerun-if-changed=include/imgui_wasm.h");
    println!("cargo:rerun-if-changed=include/imgui_wasm_imgui.h");
    println!("cargo:rerun-if-changed=build.rs");
    // Frontend assets are include_str!/include_bytes!'d into server.rs, so any
    // change must trigger a rebuild of the crate (cargo does not track
    // include_str! inputs automatically).
    println!("cargo:rerun-if-changed=frontend/index.html");
    println!("cargo:rerun-if-changed=frontend/imgui_wasm.js");
    println!("cargo:rerun-if-changed=frontend/imgui_wasm_callstream.js");
    println!("cargo:rerun-if-changed=frontend/style.css");
    println!("cargo:rerun-if-changed=wasm/imgui_wasm_replay.js");
    println!("cargo:rerun-if-changed=wasm/imgui_wasm_replay.wasm");
    for file in [
        "imgui.cpp",
        "imgui_draw.cpp",
        "imgui_tables.cpp",
        "imgui_widgets.cpp",
        "imgui_demo.cpp",
    ] {
        println!("cargo:rerun-if-changed={}", imgui_dir.join(file).display());
    }

    if !imgui_dir.exists() {
        let third_party = Path::new(&manifest).join("third_party");
        if !third_party.exists() {
            std::fs::create_dir_all(&third_party).unwrap();
        }
        println!("cargo:warning=Fetching pinned Dear ImGui ({IMGUI_REVISION})...");
        let status = Command::new("git")
            .args(["init", imgui_dir.to_str().unwrap()])
            .current_dir(&manifest)
            .status()
            .expect("Failed to initialize Dear ImGui checkout");
        if !status.success() {
            panic!("Failed to initialize Dear ImGui checkout");
        }
        let status = Command::new("git")
            .args([
                "-C",
                imgui_dir.to_str().unwrap(),
                "fetch",
                "--depth",
                "1",
                "https://github.com/ocornut/imgui.git",
                IMGUI_REVISION,
            ])
            .current_dir(&manifest)
            .status()
            .expect("Failed to fetch Dear ImGui revision");
        if !status.success() {
            panic!("Failed to fetch Dear ImGui revision");
        }
        let status = Command::new("git")
            .args(["-C", imgui_dir.to_str().unwrap(), "checkout", "FETCH_HEAD"])
            .current_dir(&manifest)
            .status()
            .expect("Failed to check out Dear ImGui revision");
        if !status.success() {
            panic!("Failed to check out Dear ImGui revision");
        }
    }

    if cfg!(target_os = "linux") {
        println!("cargo:rustc-link-lib=dylib=stdc++");
        println!("cargo:rustc-link-lib=dylib=m");
    } else if cfg!(target_os = "macos") {
        println!("cargo:rustc-link-lib=framework=CoreFoundation");
    }

    cc::Build::new()
        .cpp(true)
        .file("src/imgui_backend.cpp")
        .file("src/imgui_wasm_imgui.cpp")
        .file(imgui_dir.join("imgui.cpp"))
        .file(imgui_dir.join("imgui_draw.cpp"))
        .file(imgui_dir.join("imgui_tables.cpp"))
        .file(imgui_dir.join("imgui_widgets.cpp"))
        .file(imgui_dir.join("imgui_demo.cpp"))
        .include("include")
        .include(&imgui_dir)
        .compile("imgui_wasm_imgui_backend");

    // Build the browser WASM replay twin on every Rust build. This produces:
    //   imgui-wasm-core/wasm/imgui_wasm_replay.wasm
    //   imgui-wasm-core/wasm/imgui_wasm_replay.js   (emscripten MODULARIZE glue)
    // which are embedded into the frontend (server.rs serves them as static
    // assets). Emscripten is therefore a required build dependency.
    build_wasm_twin(&manifest, &imgui_dir);
}

fn build_wasm_twin(manifest: &str, imgui_dir: &Path) {
    let wasm_out = Path::new(manifest).join("wasm");
    // Locate emcc. Allow override via IMGUI_WASM_EMCC for non-standard installs.
    let emcc = env::var("IMGUI_WASM_EMCC").unwrap_or_else(|_| "emcc".to_string());
    // The generated replay switch + this backend + the same ImGui sources the
    // native build uses. -Os for size; wasm32 needs no special link libs.
    let mut cmd = Command::new(&emcc);
    cmd.arg("-Os");
    cmd.arg("-std=c++17");
    cmd.arg("-I").arg(Path::new(manifest).join("include"));
    cmd.arg("-I").arg(imgui_dir);
    cmd.arg("-I").arg(&wasm_out); // for generated/replay_switch.cpp's includes
    cmd.arg("-o").arg(wasm_out.join("imgui_wasm_replay.js"));
    cmd.arg(wasm_out.join("imgui_wasm_web_backend.cpp"));
    cmd.arg(wasm_out.join("generated").join("replay_switch.cpp"));
    for f in [
        "imgui.cpp",
        "imgui_draw.cpp",
        "imgui_tables.cpp",
        "imgui_widgets.cpp",
        "imgui_demo.cpp",
    ] {
        cmd.arg(imgui_dir.join(f));
    }
    // Emscripten glue: MODULARIZE with EXPORT_NAME emits a factory assigned to
    // a global (var imgui_wasm_replay = ...). The frontend loads it via a <script>
    // tag and awaits the factory() Promise. We deliberately do NOT use
    // EXPORT_ES6: that mode drops the global assignment in favor of `export
    // default`, but the emscripten runtime still isn't a real ES module.
    cmd.args([
        "-Os",
        "-s",
        "MODULARIZE=1",
        "-s",
        "EXPORT_NAME=imgui_wasm_replay",
        "-s",
        "ALLOW_MEMORY_GROWTH=1",
        "-s",
        "EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAP32']",
        "-s",
        "EXPORTED_FUNCTIONS=[
            _malloc,
            _free,
            _imgui_wasm_replay_init,
            _imgui_wasm_replay_set_display_size,
            _imgui_wasm_replay_set_string,
            _imgui_wasm_replay_frame,
            _imgui_wasm_replay_draw_data_len,
            _imgui_wasm_replay_list_count,
            _imgui_wasm_replay_font_tex_width,
            _imgui_wasm_replay_font_tex_height,
            _imgui_wasm_replay_font_tex_pixels,
            _imgui_wasm_replay_input_mouse_pos,
            _imgui_wasm_replay_input_mouse_down,
            _imgui_wasm_replay_input_mouse_up,
            _imgui_wasm_replay_input_wheel,
            _imgui_wasm_replay_input_key,
            _imgui_wasm_replay_input_char
        ]",
        "-s",
        "ENVIRONMENT=web",
    ]);

    println!("cargo:warning=Building ImGuiWasm WASM replay twin (emcc)...");
    let status = cmd.current_dir(manifest).status().unwrap_or_else(|e| {
        panic!(
            "Failed to invoke emcc ({}): {}. Is emscripten installed and activated?",
            emcc, e
        )
    });
    if !status.success() {
        panic!("emcc failed building the WASM replay twin");
    }

    // Track the WASM sources so a change triggers rebuild.
    println!("cargo:rerun-if-changed=wasm/imgui_wasm_web_backend.cpp");
    println!("cargo:rerun-if-changed=wasm/generated/replay_switch.cpp");
    println!("cargo:rerun-if-changed=include/imgui_wasm_opcodes.h");
    println!("cargo:rerun-if-env-changed=IMGUI_WASM_EMCC");
}
