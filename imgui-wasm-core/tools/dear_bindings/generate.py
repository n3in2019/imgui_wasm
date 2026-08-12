#!/usr/bin/env python3
"""Regenerate Dear ImGui C bindings and all downstream language artifacts."""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
IMGUI_REVISION = "b61e56346a92cfcaf1f43a545ca37b0b32239654"
DEAR_BINDINGS_REVISION = "c9ff64913915df41c0f4beef485b98a1c685eda5"


def run(*args: str, cwd: Path | None = None) -> None:
    subprocess.run(args, cwd=cwd, check=True)


def fetch_revision(url: str, revision: str, destination: Path) -> None:
    run("git", "init", str(destination))
    run("git", "-C", str(destination), "fetch", "--depth", "1", url, revision)
    run("git", "-C", str(destination), "checkout", "--detach", "FETCH_HEAD")


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="imgui-wasm-bindings-") as temporary:
        temp = Path(temporary)
        imgui = temp / "imgui"
        generator = temp / "dear_bindings"
        environment = temp / "venv"
        output = temp / "imgui_wasm_imgui"

        fetch_revision("https://github.com/ocornut/imgui.git", IMGUI_REVISION, imgui)
        fetch_revision(
            "https://github.com/dearimgui/dear_bindings.git",
            DEAR_BINDINGS_REVISION,
            generator,
        )
        run(sys.executable, "-m", "venv", str(environment))
        python = environment / "bin" / "python"
        if sys.platform == "win32":
            python = environment / "Scripts" / "python.exe"
        run(str(python), "-m", "pip", "install", "-r", str(generator / "requirements.txt"))
        run(
            str(python),
            str(generator / "dear_bindings.py"),
            "--custom-namespace-prefix",
            "ig",
            "--nogeneratedefaultargfunctions",
            "-o",
            str(output),
            str(imgui / "imgui.h"),
        )

        include = ROOT / "imgui-wasm-core" / "include" / "imgui_wasm_imgui.h"
        source = ROOT / "imgui-wasm-core" / "src" / "imgui_wasm_imgui.cpp"
        metadata = Path(__file__).with_name("imgui_wasm_imgui.json")
        shutil.copyfile(output.with_suffix(".h"), include)
        shutil.copyfile(output.with_suffix(".cpp"), source)
        shutil.copyfile(output.with_suffix(".json"), metadata)

    run(sys.executable, str(ROOT / "imgui-wasm-python" / "tools" / "generate_bindings.py"))


if __name__ == "__main__":
    main()
