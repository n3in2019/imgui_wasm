from __future__ import annotations

import ctypes
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional


@dataclass
class Config:
    host_port: str = "127.0.0.1:8888"
    compression: bool = False
    config_flags: int = 1 << 7
    dark_style: bool = True
    transport: int = 0


class _CConfig(ctypes.Structure):
    _fields_ = [
        ("host_port", ctypes.c_char_p),
        ("compression", ctypes.c_int),
        ("config_flags", ctypes.c_int),
        ("dark_style", ctypes.c_int),
        ("transport", ctypes.c_int),
    ]


_lib: Optional[ctypes.CDLL] = None


def find_library() -> Path:
    env = os.environ.get("IMGUI_WASM_LIBRARY")
    if env:
        return Path(env)

    here = Path(__file__).resolve()
    candidates = [
        here.parents[2] / "target" / "debug" / _library_name(),
        here.parents[2] / "target" / "release" / _library_name(),
        here.parents[3] / "target" / "debug" / _library_name(),
        here.parents[3] / "target" / "release" / _library_name(),
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise FileNotFoundError(
        "Could not find libimgui_wasm. Set IMGUI_WASM_LIBRARY to the built shared library path."
    )


def _library_name() -> str:
    if sys.platform == "darwin":
        return "libimgui_wasm.dylib"
    if os.name == "nt":
        return "imgui_wasm.dll"
    return "libimgui_wasm.so"


def _load() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        lib = ctypes.CDLL(str(find_library()))
        lib.imgui_wasm_init.argtypes = [ctypes.POINTER(_CConfig)]
        lib.imgui_wasm_init.restype = ctypes.c_int
        lib.imgui_wasm_shutdown.argtypes = []
        lib.imgui_wasm_shutdown.restype = None
        lib.imgui_wasm_new_frame.argtypes = []
        lib.imgui_wasm_new_frame.restype = None
        lib.imgui_wasm_render.argtypes = []
        lib.imgui_wasm_render.restype = None
        _lib = lib
    return _lib


def init(config: Optional[Config] = None) -> None:
    cfg = config or Config()
    c_config = _CConfig(
        cfg.host_port.encode("utf-8"),
        1 if cfg.compression else 0,
        int(cfg.config_flags),
        1 if cfg.dark_style else 0,
        int(cfg.transport),
    )
    rc = _load().imgui_wasm_init(ctypes.byref(c_config))
    if rc != 0:
        raise RuntimeError(f"imgui_wasm_init failed with code {rc}")


def shutdown() -> None:
    _load().imgui_wasm_shutdown()


def new_frame() -> None:
    _load().imgui_wasm_new_frame()


def render() -> None:
    _load().imgui_wasm_render()


class Server:
    def __init__(self, config: Optional[Config] = None):
        self.config = config or Config()
        self.initialized = False

    def init(self) -> None:
        if not self.initialized:
            init(self.config)
            self.initialized = True

    def shutdown(self) -> None:
        if self.initialized:
            shutdown()
            self.initialized = False

    def new_frame(self) -> None:
        if not self.initialized:
            raise RuntimeError("Server.init() must be called before new_frame()")
        new_frame()

    def render(self) -> None:
        if not self.initialized:
            raise RuntimeError("Server.init() must be called before render()")
        render()

    def frame(self, draw: Callable[[], None]) -> None:
        self.new_frame()
        draw()
        self.render()

    def __enter__(self) -> "Server":
        self.init()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.shutdown()
