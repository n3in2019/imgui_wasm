# imgui-wasm-python

Python access to the ImGuiWasm core runtime.

The stable core ABI is intentionally small:

```python
import imgui_wasm

server = imgui_wasm.Server()
server.init()

while True:
    server.new_frame()
    visible, _ = imgui_wasm.begin("Python")
    if visible:
        imgui_wasm.text("Hello from Python")
    imgui_wasm.end()
    server.render()
```

## ImGui API

`imgui_wasm.core` is the small handwritten lifecycle layer. `imgui_wasm.imgui` is generated
from `dear_bindings` metadata and calls ImGuiWasm-owned `imgui_wasm_ig*` exports, so Python does
not need `pyimgui`.
