#!/usr/bin/env python3
"""Bump the vendored Dear ImGui revision and regenerate everything downstream.

One command for the upstream-tracking workflow:

  1. resolve and fetch the new ocornut/imgui revision into third_party/imgui
  2. re-pin it in CMakeLists.txt, tools/dear_bindings/generate.py and
     ports/imgui-wasm/portfile.cmake
  3. regenerate the C bindings (tools/dear_bindings/generate.py) and the
     call-stream artifacts (tools/generate_bindings.py)
  4. rebuild the WASM replay twin (tools/build_wasm_twin.py; needs em++)
  5. rebuild the core and run the unit tests

Re-running with the current pin is a valid end-to-end pipeline check.

Usage:
  python3 tools/bump_upstream.py <imgui-revision> [--dear-bindings-rev <rev>]
                                 [--skip-twin] [--build-dir build]

<imgui-revision> may be a tag (v1.92.9-docking), a branch (docking), or a
40-char commit SHA. Every step needs network access; the twin additionally
needs em++ on PATH (or IMGUI_WASM_EMCC).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMGUI_URL = "https://github.com/ocornut/imgui.git"
IMGUI_DIR = ROOT / "third_party" / "imgui"

# Exactly one revision pin per file; the portfile pattern targets the
# vcpkg_from_git block for ocornut/imgui, not the port's own source REF.
PIN_FILES = [
    (ROOT / "CMakeLists.txt", r'(set\(IMGUI_REVISION ")[0-9a-f]{40}(")'),
    (ROOT / "tools" / "dear_bindings" / "generate.py", r'(IMGUI_REVISION = ")[0-9a-f]{40}(")'),
    (ROOT / "ports" / "imgui-wasm" / "portfile.cmake",
     r'(ocornut/imgui\.git"\s*\n\s*REF ")[0-9a-f]{40}(")'),
]


def run(*args: object) -> None:
    print(f"+ {' '.join(str(a) for a in args)}", flush=True)
    subprocess.run([str(a) for a in args], check=True)


def resolve_revision(rev: str) -> str:
    """Resolve a tag/branch name to its full commit SHA (SHAs pass through)."""
    if re.fullmatch(r"[0-9a-f]{40}", rev):
        return rev
    ls = subprocess.run(["git", "ls-remote", IMGUI_URL], capture_output=True, text=True,
                        check=True).stdout
    shas = set()
    for line in ls.splitlines():
        sha, _, ref = line.partition("\t")
        if ref.endswith("^{}"):
            continue  # peeled-tag duplicates
        if ref == rev or ref.endswith("/" + rev):
            shas.add(sha)
    if len(shas) != 1:
        candidates = "\n  ".join(sorted(
            line.split("\t")[1] for line in ls.splitlines() if rev in line))
        sys.exit(f"revision '{rev}' matched {len(shas)} refs. Candidates:\n  {candidates or '(none)'}")
    return shas.pop()


def update_checkout(sha: str) -> None:
    """Move third_party/imgui to the SHA; CMake's fetch only runs when missing."""
    if not (IMGUI_DIR / "imgui.h").exists():
        sys.exit(f"Dear ImGui not fetched at {IMGUI_DIR}; run 'cmake -B build' once first.")
    run("git", "-C", IMGUI_DIR, "fetch", "--depth", "1", IMGUI_URL, sha)
    run("git", "-C", IMGUI_DIR, "checkout", "--detach", "FETCH_HEAD")


def re_pin(sha: str, pattern: str, path: Path, label: str) -> None:
    text = path.read_text()
    new_text, count = re.subn(pattern, r"\g<1>" + sha + r"\g<2>", text)
    if count != 1:
        sys.exit(f"expected exactly one {label} pin in {path}, found {count}")
    path.write_text(new_text)
    print(f"pinned {path.relative_to(ROOT)}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("revision", help="ocornut/imgui tag, branch, or 40-char SHA")
    parser.add_argument("--dear-bindings-rev",
                        help="also re-pin DEAR_BINDINGS_REVISION in generate.py")
    parser.add_argument("--skip-twin", action="store_true",
                        help="skip the Emscripten twin rebuild")
    parser.add_argument("--build-dir", default="build")
    args = parser.parse_args()

    sha = resolve_revision(args.revision)
    print(f"Bumping Dear ImGui -> {sha}")
    update_checkout(sha)
    for path, pattern in PIN_FILES:
        re_pin(sha, pattern, path, "IMGUI_REVISION")
    if args.dear_bindings_rev:
        re_pin(args.dear_bindings_rev, r'(DEAR_BINDINGS_REVISION = ")[0-9a-f]{40}(")',
               ROOT / "tools" / "dear_bindings" / "generate.py", "DEAR_BINDINGS_REVISION")

    run(sys.executable, ROOT / "tools" / "dear_bindings" / "generate.py")
    if not args.skip_twin:
        run(sys.executable, ROOT / "tools" / "build_wasm_twin.py")

    run("cmake", "--build", ROOT / args.build_dir)
    tests = ROOT / args.build_dir / "tests" / "core_cpp_tests"
    if tests.exists():
        run(tests)

    print(
        "\nUpstream bump complete. Review with 'git status' / 'git diff' — expected\n"
        "changes: the three pins, the regenerated bindings (include/src\n"
        "imgui_wasm_imgui.*, tools/dear_bindings/*.json, tools/generated/*,\n"
        "include/imgui_wasm_opcodes.h, include/imgui_wasm_capture.hpp,\n"
        "wasm/generated/replay_switch.cpp) and the rebuilt twin\n"
        "(wasm/imgui_wasm_replay.{js,wasm}). Commit the twin artifacts together\n"
        "with the pins. At release time, refresh the port version and the\n"
        "versions/ git-tree as usual."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
