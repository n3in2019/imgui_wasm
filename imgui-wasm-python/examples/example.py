import time

import imgui_wasm


show_demo = True
running = True
value = 0.0
counter = 0
last_time = time.perf_counter()
fps = 60.0
choice = 0
choices = ["Alpha", "Beta", "Gamma"]


def draw():
    global choice, counter, fps, last_time, running, show_demo, value

    now = time.perf_counter()
    delta = max(now - last_time, 0.001)
    last_time = now
    fps = (fps * 0.9) + ((1.0 / delta) * 0.1)

    imgui_wasm.dock_space_over_viewport()

    if show_demo:
        show_demo = imgui_wasm.show_demo_window(show_demo)

    visible, running = imgui_wasm.begin("ImGuiWasm Python Example", running)
    if visible:
        imgui_wasm.text("Hello from Python through ImGuiWasm's ImGui context.")
        _, show_demo = imgui_wasm.checkbox("Show Demo Window", show_demo)
        _, value = imgui_wasm.slider_float("float", value, 0.0, 1.0)
        if imgui_wasm.begin_combo("Choice", choices[choice], 0):
            for index, label in enumerate(choices):
                if imgui_wasm.selectable_bool(label, choice == index, 0, imgui_wasm.imgui.ImVec2(0.0, 0.0)):
                    choice = index
            imgui_wasm.end_combo()

        if imgui_wasm.button("Button"):
            counter += 1
        imgui_wasm.same_line()
        imgui_wasm.text(f"counter = {counter}")
        imgui_wasm.text(f"Application average {1000.0 / max(fps, 0.001):.3f} ms/frame")
    imgui_wasm.end()


def main():
    with imgui_wasm.Server() as server:
        while running:
            server.frame(draw)
            time.sleep(1.0 / 60.0)


if __name__ == "__main__":
    main()
