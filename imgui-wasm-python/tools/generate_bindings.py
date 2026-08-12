#!/usr/bin/env python3
from __future__ import annotations

import json
import keyword
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEAR_BINDINGS_METADATA = ROOT / "imgui-wasm-core" / "tools" / "dear_bindings" / "imgui_wasm_imgui.json"
OUT_PY = ROOT / "imgui-wasm-python" / "imgui_wasm" / "imgui.py"
OUT_RS = ROOT / "imgui-wasm-core" / "src" / "generated_imgui_api.rs"

# --- Call-stream transport outputs (Phase 0+) -------------------------------
# Stable opcode space + per-opcode arg schema. Consumed by:
#   - build.rs ImGui patcher (capture shims)        -> patch_specs.json
#   - the browser WASM replay twin                  -> replay_switch.cpp
#   - the Rust wire framer                          -> callstream_schema.json
OUT_CS_SCHEMA = ROOT / "imgui-wasm-core" / "tools" / "generated" / "callstream_schema.json"
OUT_CS_OPCODES_H = ROOT / "imgui-wasm-core" / "include" / "imgui_wasm_opcodes.h"
OUT_CS_PATCH_SPECS = ROOT / "imgui-wasm-core" / "tools" / "generated" / "patch_specs.json"
OUT_CS_REPLAY_SWITCH = ROOT / "imgui-wasm-core" / "wasm" / "generated" / "replay_switch.cpp"
OUT_CS_CAPTURE_SHIMS = ROOT / "imgui-wasm-core" / "wasm" / "generated" / "capture_shims.h"
# The host-facing capture header: imgui_wasm::<Widget> wrappers that record then
# forward to ImGui::<Widget>. This is THE capture point. Hosts include this
# instead of calling ImGui:: directly (C++), or use the ig* ABI (Python/FFI).
OUT_CS_CAPTURE_HPP = ROOT / "imgui-wasm-core" / "include" / "imgui_wasm_capture.hpp"
CS_OPCODE_MANIFEST = ROOT / "imgui-wasm-core" / "tools" / "callstream_opcodes.json"

PY_BASE_TYPES = {
    "bool": "ctypes.c_bool",
    "char": "ctypes.c_char",
    "char*": "ctypes.c_char_p",
    "const char*": "ctypes.c_char_p",
    "double": "ctypes.c_double",
    "float": "ctypes.c_float",
    "int": "ctypes.c_int",
    "size_t": "ctypes.c_size_t",
    "unsigned char": "ctypes.c_ubyte",
    "unsigned int": "ctypes.c_uint",
    "unsigned short": "ctypes.c_ushort",
    "void": "None",
    "void*": "ctypes.c_void_p",
    "const void*": "ctypes.c_void_p",
    "ImGuiID": "ctypes.c_uint",
    "ImS64": "ctypes.c_int64",
    "ImU8": "ctypes.c_uint8",
    "ImU16": "ctypes.c_uint16",
    "ImU32": "ctypes.c_uint32",
    "ImU64": "ctypes.c_uint64",
    "ImTextureID": "ctypes.c_void_p",
    "ImWchar": "ctypes.c_uint16",
    "ImWchar32": "ctypes.c_uint32",
}

RUST_BASE_TYPES = {
    "bool": "bool",
    "char": "c_char",
    "char*": "*mut c_char",
    "const char*": "*const c_char",
    "double": "c_double",
    "float": "f32",
    "int": "c_int",
    "size_t": "usize",
    "unsigned char": "c_uchar",
    "unsigned int": "c_uint",
    "unsigned short": "c_ushort",
    "void": "()",
    "void*": "*mut c_void",
    "const void*": "*const c_void",
    "ImGuiID": "c_uint",
    "ImS64": "i64",
    "ImU8": "u8",
    "ImU16": "u16",
    "ImU32": "u32",
    "ImU64": "u64",
    "ImTextureID": "*mut c_void",
    "ImWchar": "u16",
    "ImWchar32": "u32",
}

STRUCTS = {
    "ImVec2": [("x", "ctypes.c_float", "f32"), ("y", "ctypes.c_float", "f32")],
    "ImVec4": [
        ("x", "ctypes.c_float", "f32"),
        ("y", "ctypes.c_float", "f32"),
        ("z", "ctypes.c_float", "f32"),
        ("w", "ctypes.c_float", "f32"),
    ],
}

PY_DEFAULTS = {
    "NULL": "None",
    "((void*)0)": "None",
    "0": "0",
    "0.0f": "0.0",
    "1.0f": "1.0",
    "-1.0f": "-1.0",
    "false": "False",
    "true": "True",
    '"%.3f"': '"%.3f"',
    "ImVec2(0,0)": "ImVec2(0.0, 0.0)",
    "ImVec2(-FLT_MIN,0.0f)": "ImVec2(-3.4028234663852886e38, 0.0)",
    "ImVec4(0,0,0,0)": "ImVec4(0.0, 0.0, 0.0, 0.0)",
}


def clean_type(type_name: str) -> str:
    return type_name.replace("_c", "").replace("*const[]", "* const[]").strip()


def enum_type(type_name: str) -> bool:
    return type_name.startswith("ImGui") and type_name.endswith("Flags")


def py_ctype(type_name: str) -> str | None:
    t = clean_type(type_name)
    if t in PY_BASE_TYPES:
        return PY_BASE_TYPES[t]
    if enum_type(t) or t in {"ImGuiDir", "ImGuiKey", "ImGuiMouseButton", "ImGuiMouseCursor", "ImGuiSortDirection"}:
        return "ctypes.c_int"
    if t in STRUCTS:
        return t
    if t.startswith("const ") and t[6:] in STRUCTS:
        return t[6:]
    if t.endswith("Callback"):
        return "ctypes.c_void_p"
    if t.endswith("[3]") or t.endswith("[4]"):
        base = t.rsplit("[", 1)[0]
        if base == "float":
            return "ctypes.POINTER(ctypes.c_float)"
    if t == "const char* const[]":
        return "ctypes.POINTER(ctypes.c_char_p)"
    if t.endswith("*"):
        base = t[:-1].strip()
        if base.startswith("const "):
            base = base[6:].strip()
        if base in PY_BASE_TYPES and PY_BASE_TYPES[base] != "None":
            return f"ctypes.POINTER({PY_BASE_TYPES[base]})"
        return "ctypes.c_void_p"
    return None


def rust_type(type_name: str) -> str | None:
    t = clean_type(type_name)
    if t in RUST_BASE_TYPES:
        return RUST_BASE_TYPES[t]
    if enum_type(t) or t in {"ImGuiDir", "ImGuiKey", "ImGuiMouseButton", "ImGuiMouseCursor", "ImGuiSortDirection"}:
        return "c_int"
    if t in STRUCTS:
        return t
    if t.startswith("const ") and t[6:] in STRUCTS:
        return t[6:]
    if t.endswith("Callback"):
        return "*mut c_void"
    if t.endswith("[3]") or t.endswith("[4]"):
        base = t.rsplit("[", 1)[0]
        if base == "float":
            return "*mut f32"
    if t == "const char* const[]":
        return "*const *const c_char"
    if t.endswith("*"):
        is_const = t.startswith("const ")
        base = t[:-1].strip()
        if base.startswith("const "):
            base = base[6:].strip()
        if base in RUST_BASE_TYPES and RUST_BASE_TYPES[base] != "()":
            return f"*{'const' if is_const else 'mut'} {RUST_BASE_TYPES[base]}"
        return "*mut c_void" if not t.startswith("const ") else "*const c_void"
    return None


# ---------------------------------------------------------------------------
# Call-stream transport: stable opcode table + per-opcode arg schema.
#
# A "call-stream" replaces per-frame vertex/index transport. The server
# captures the sequence of ImGui API calls the host makes; the browser replays
# them against a WASM-compiled Dear ImGui to regenerate draw data locally.
#
# Correctness model (server-authoritative echo):
#   - The server ImGui is authoritative. Host logic and output-pointer writes
#     (SliderFloat's &v, Checkbox's &v, ...) live server-side.
#   - The browser runs a twin ImGui. It receives the same io input the server
#     does and replays the call sequence. For every output pointer the server
#     serializes the pointee's CURRENT value; the browser supplies a scratch
#     pointer and discards the twin's own write. Next frame the server echoes
#     the (possibly mutated) value, so both contexts converge.
#
# This block classifies every arg into a serialization category. The schema is
# the single source of truth shared by the Rust framer, the WASM replay switch,
# and the C++ capture shim generator. Categories:
#
#   scalar          bool/char/int/float/double/size_t/ImU*/ImS*/ImGuiID/ImWchar
#                   -> fixed-width LE in place
#   enum            ImGui* enum (resolved from structs_and_enums.json)
#                   -> u32 LE value
#   string          const char* / char* (read-only label)
#                   -> interned id u32 (string table sent once)
#   vec2 / vec4     ImVec2/4 by value, or const ImVec2* (dereferenced)
#                   -> 2 or 4 floats LE
#   floatarr_in     const float* (+ sibling count) -> count u32 + floats
#   strarr_in       const char* const[] (+ sibling count) -> count u32 + ids
#   floatarr        float[N] (fixed, inout; server echoes current values)
#                   -> N floats LE  (N = 3 or 4)
#   ptr_out_scalar  T* (T in {float,bool,int,uint,double,size_t}) inout
#                   -> 1 scalar LE (server-authoritative current value)
#   ptr_buf         char* buf + size_t buf_size pair (InputText etc.)
#                   -> len u32 + bytes; on echo the server sends current content
#
# Anything that cannot be proxied (opaque object pointers like ImFont* /
# ImGuiContext*, function pointers / callbacks, void*) causes the function to
# be EXCLUDED from the call-stream surface; the draw-data fallback covers it.

# Scalar types: maps cleaned binding type -> (width_bytes, cpp_type, rust_type).
# Width drives the wire encoding; cpp_type/rust_type drive codegen so floats
# stay floats and ints stay ints (both are width 4 but encode differently).
CS_SCALARS = {
    "bool":          (1, "bool",        "bool"),
    "char":          (1, "char",        "i8"),
    "signed char":   (1, "signed char", "i8"),
    "unsigned char": (1, "unsigned char", "u8"),
    "short":         (2, "short",       "i16"),
    "unsigned short":(2, "unsigned short", "u16"),
    "int":           (4, "int",         "i32"),
    "unsigned int":  (4, "unsigned int", "u32"),
    "size_t":        (4, "unsigned int", "u32"),  # wasm32
    "ImS32":         (4, "int",         "i32"),
    "ImU32":         (4, "unsigned int","u32"),
    "ImS64":         (8, "long long",   "i64"),
    "ImU64":         (8, "unsigned long long", "u64"),
    "float":         (4, "float",       "f32"),
    "double":        (8, "double",      "f64"),
    "ImGuiID":       (4, "unsigned int","u32"),
    "ImU8":          (1, "unsigned char", "u8"),
    "ImU16":         (2, "unsigned short", "u16"),
    "ImS8":          (1, "signed char",   "i8"),
    "ImS16":         (2, "short",         "i16"),
    "ImWchar":       (2, "unsigned short", "u16"),
    "ImWchar32":     (4, "unsigned int",   "u32"),
}

# Back-compat alias kept for any external reader; width-only view.
CS_SCALAR_WIDTHS = {k: v[0] for k, v in CS_SCALARS.items()}

# Hand-vetted pointer intent overrides. Generator metadata does not always record
# whether a `T*` is in, out, or inout, so the classifier uses name + type
# heuristics and this table is the authoritative correction layer. Keys are
# (symbol, arg_name); values are the category to force.
#
# This is the load-bearing correctness table for output pointers. Any new
# stateful widget added upstream must be audited here.
CS_PTR_OVERRIDES = {
    # format: (symbol, arg_name): category
    # All known inout scalar pointers in the proxyable surface are correctly
    # classified by name heuristic today (v, p_open, current_item, col, ...).
    # Add explicit entries here when an upstream change needs correction.
}

CS_ENUM_NAMES: set[str] = set()


def load_definitions() -> dict[str, list[dict]]:
    """Adapt dear_bindings metadata to the generator's compact function model."""
    metadata = json.loads(DEAR_BINDINGS_METADATA.read_text())
    definitions: dict[str, list[dict]] = {}
    for function in metadata["functions"]:
        qualified = function.get("original_fully_qualified_name", "")
        if not qualified.startswith("ImGui::"):
            continue
        args = [
            {
                "name": arg["name"],
                "type": "..." if arg.get("is_varargs") else arg["type"]["declaration"],
            }
            for arg in function["arguments"]
        ]
        defaults = {
            arg["name"]: arg["default_value"]
            for arg in function["arguments"]
            if "default_value" in arg
        }
        symbol = function["name"]
        adapted = {
            "args": "(" + ",".join(arg["type"] for arg in args) + ")",
            "argsT": args,
            "cimguiname": symbol,
            "ov_cimguiname": symbol,
            "defaults": defaults,
            "funcname": qualified.rsplit("::", 1)[-1],
            "location": f"imgui:{function.get('source_location', {}).get('line', 0)}",
            "namespace": "ImGui",
            "ret": function["return_type"]["declaration"],
        }
        definitions.setdefault(symbol, []).append(adapted)
    return definitions


def _load_enum_names() -> set[str]:
    global CS_ENUM_NAMES
    if not CS_ENUM_NAMES and DEAR_BINDINGS_METADATA.exists():
        metadata = json.loads(DEAR_BINDINGS_METADATA.read_text())
        CS_ENUM_NAMES = {enum["name"].rstrip("_") for enum in metadata["enums"]}
    return CS_ENUM_NAMES


def classify_arg(type_name: str) -> str | None:
    """Return the call-stream serialization category for an arg type, or None
    if the arg cannot be proxied (opaque pointer / callback)."""
    t = clean_type(type_name)

    # const ImVec2* / ImVec2* -> treat like by-value vec (deref + 2 floats).
    if t in ("ImVec2", "const ImVec2", "const ImVec2*", "ImVec2*"):
        return "vec2"
    if t in ("ImVec4", "const ImVec4", "const ImVec4*", "ImVec4*"):
        return "vec4"

    if t in CS_SCALARS:
        return "scalar"

    if t in ("const char*", "char*"):
        return "string"

    if t in _load_enum_names():
        return "enum"
    # A handful of plain enums are not flags-style but still u32.
    if t in {"ImGuiDir", "ImGuiKey", "ImGuiMouseButton", "ImGuiMouseCursor",
             "ImGuiSortDirection", "ImGuiCol", "ImGuiCond", "ImGuiStyleVar",
             "ImGuiDataType", "ImGuiAxis", "ImGuiLayoutType", "ImGuiTableBgTarget",
             "ImGuiInputSource", "ImGuiMouseSource", "ImGuiNavLayer", "ImGuiPlotType"}:
        return "enum"

    # Fixed-size float arrays: float[3] / float[4]. These are inout (ColorEdit).
    if t.endswith("[3]") or t.endswith("[4]"):
        base = t.rsplit("[", 1)[0].strip()
        if base == "float":
            return "floatarr"
        return None

    # Input array of strings: const char* const[] (Combo/ListBox items).
    if t == "const char* const[]":
        return "strarr_in"
    # Input float array with explicit count + stride (PlotLines etc.).
    if t == "const float*":
        return "floatarr_in"

    # Output / inout scalar pointers (server-authoritative echo).
    if t in ("float*", "bool*", "int*", "unsigned int*", "double*", "size_t*",
             "ImS32*", "ImU32*", "ImS64*", "ImU64*"):
        return "ptr_out_scalar"
    if t == "char*":
        return "ptr_buf"

    # Everything else (opaque object pointers, function pointers, void*) is
    # not proxyable -> caller excludes the function.
    return None


def schema_for_arg(arg: dict, symbol: str) -> dict | None:
    """Build a serializable schema node for one arg, or None if unproxyable.
    Applies the hand-vetted override table."""
    name = arg.get("name", "")
    t = clean_type(arg["type"])
    override = CS_PTR_OVERRIDES.get((symbol, name))
    cat = override if override else classify_arg(t)
    if cat is None:
        return None
    node = {"name": name, "type": arg["type"], "cat": cat}
    if cat == "scalar":
        width, cpp, rust = CS_SCALARS[t]
        node["width"] = width
        node["cpp_type"] = cpp
        node["rust_type"] = rust
    elif cat == "floatarr":
        node["len"] = int(t[-2])
    elif cat == "ptr_out_scalar":
        # Width of the pointee, so capture and replay agree on byte count.
        # Mirrors CS_SCALARS for the base type.
        bt = t.rstrip("*")
        node["width"] = CS_SCALARS[bt][0]
    return node


def callstream_supported(function: dict) -> bool:
    """Stricter than supported(): every arg must be proxyable AND no variadics.
    Excludes callback/opaque-pointer functions.

    Also excludes pure query/getter/compute functions: the browser replay twin
    only needs to reproduce the *drawing* call sequence. Getters (igGet*), input
    queries (igIs*), and color/math conversions (igColorConvert*, igCalc*) are
    host-side helpers whose return values feed host logic, not geometry. The
    server ImGui is authoritative for those; replaying them in the twin would
    be both pointless and problematic (float& out-params, struct returns).
    igCalcItemWidth/igCalcTextSize are layout queries the host calls between
    widgets to decide sizing; the twin reproduces layout by replaying the
    widgets themselves, so it does not need these.

    Finally, a counted-array arg (floatarr_in/strarr_in) must have a resolvable
    count sibling (an int arg whose name contains 'count' or is 'n'). A bare
    `const float*` with no count (e.g. ColorPicker4's ref_col) is ambiguous in
    the metadata and excluded rather than emitted as broken code."""
    if not supported(function):
        return False
    if "..." in function.get("args", ""):
        return False
    sym = symbol_name(function)
    QUERY_PREFIXES = (
        "igGet",          # igGetMousePos, igGetCursorPos, igGetIO, ...
        "igIs",           # igIsItemHovered, igIsKeyDown, ...
        "igColorConvert", # igColorConvertHSVtoRGB, ... (float& out-params)
        "igCalc",         # igCalcItemWidth, igCalcTextSize (layout queries)
    )
    if sym.startswith(QUERY_PREFIXES):
        return False
    args = function["argsT"]
    # Every counted-array arg needs a resolvable count sibling.
    for arg in args:
        node = schema_for_arg(arg, sym)
        if node is None:
            return False
        if node["cat"] in ("floatarr_in", "strarr_in"):
            if not any(
                a != arg
                and schema_for_arg(a, sym)
                and schema_for_arg(a, sym)["cat"] == "scalar"
                and ("count" in a.get("name", "") or a.get("name") == "n")
                for a in args
            ):
                return False
    return True


def collect_callstream_functions(definitions: dict) -> list[dict]:
    """All functions in the call-stream surface, in stable sorted order."""
    funcs: dict[str, dict] = {}
    for overloads in definitions.values():
        for fn in overloads:
            if callstream_supported(fn):
                funcs[symbol_name(fn)] = fn
    return [funcs[name] for name in sorted(funcs)]


def build_callstream_schema(functions: list[dict]) -> dict:
    """Build the schema while preserving permanent manifest opcode values."""
    assigned = json.loads(CS_OPCODE_MANIFEST.read_text())
    values = list(assigned.values())
    if any(not isinstance(value, int) or not 0 < value <= 0xFFFF for value in values):
        raise ValueError("call-stream opcodes must be non-zero u16 values")
    if len(values) != len(set(values)):
        raise ValueError("call-stream opcode values must be unique")
    opcodes = {}
    ops = []
    for fn in functions:
        sym = symbol_name(fn)
        args = []
        for a in fn["argsT"]:
            node = schema_for_arg(a, sym)
            assert node is not None, f"unproxyable arg leaked into schema: {sym}.{a}"
            args.append(node)
        key = semantic_opcode_key(fn, args)
        opcode = assigned.get(key)
        if opcode is None:
            # New semantic calls require an explicit manifest assignment and
            # a rebuilt WASM replay twin. Do not allocate protocol values from
            # generator ordering.
            continue
        opcodes[sym] = opcode
        ops.append({
            "opname": sym,
            "opcode": opcode,
            # funcname = the real ImGui C++ method name (e.g. "Combo" for
            # an overloaded Combo variant). cimguiname = the C symbol.
            # Both are needed: replay calls ImGui::<funcname>; the capture
            # patch renames ImGui::<funcname>'s definition.
            "funcname": fn.get("funcname") or sym,
            "cimguiname": fn.get("cimguiname") or sym,
            "ret": clean_type(fn["ret"]),
            "location": fn.get("location", ""),
            "args": args,
            "defaults": fn.get("defaults", {}),
        })
    ops.sort(key=lambda op: op["opcode"])
    return {"opcodes": opcodes, "ops": ops}


def semantic_opcode_key(function: dict, args: list[dict]) -> str:
    """Identify a call independently of C-generator overload spelling."""
    funcname = function.get("funcname") or symbol_name(function)
    signature = []
    for arg in args:
        type_name = clean_type(arg.get("type", "")).replace("const ImVec2", "ImVec2").replace(
            "const ImVec4", "ImVec4"
        )
        signature.append(f"{arg['cat']}:{type_name}")
    return f"{funcname}({','.join(signature)})"


def symbol_name(function: dict) -> str:
    return function.get("ov_cimguiname") or function["cimguiname"]


def supported(function: dict) -> bool:
    if function.get("templated"):
        return False
    if function.get("constructor") or function.get("destructor"):
        return False
    if function.get("namespace") != "ImGui":
        return False
    if not symbol_name(function).startswith("ig"):
        return False
    if not str(function.get("location", "")).startswith("imgui:"):
        return False
    if "..." in function.get("args", ""):
        return False
    if py_ctype(function["ret"]) is None or rust_type(function["ret"]) is None:
        return False
    return all(py_ctype(arg["type"]) is not None and rust_type(arg["type"]) is not None for arg in function["argsT"])


def collect_functions(definitions: dict) -> list[dict]:
    functions: dict[str, dict] = {}
    for overloads in definitions.values():
        for fn in overloads:
            if supported(fn):
                functions[symbol_name(fn)] = fn
    return [functions[name] for name in sorted(functions)]


def snake(name: str) -> str:
    name = name[2:] if name.startswith("ig") else name
    name = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    return name.replace("__", "_").lower()


def py_name(raw: str) -> str:
    name = snake(raw)
    if keyword.iskeyword(name):
        return f"{name}_"
    return name


def py_arg_name(name: str, index: int) -> str:
    name = name or f"arg{index}"
    name = re.sub(r"\W+", "_", name).strip("_")
    if not name or keyword.iskeyword(name):
        name = f"{name}_arg" if name else f"arg{index}"
    return name


def py_default(value: str) -> str | None:
    return PY_DEFAULTS.get(value)


def emit_python_function(fn: dict) -> list[str]:
    sym = symbol_name(fn)
    args = []
    call_args = []
    required = []
    optional = []
    defaults = fn.get("defaults", {})
    for index, arg in enumerate(fn["argsT"]):
        name = py_arg_name(arg["name"], index)
        default = py_default(defaults[arg["name"]]) if arg["name"] in defaults else None
        if default is None:
            required.append(name)
        else:
            optional.append(f"{name}={default}")
        args.append(name)
        call_args.append(f"_coerce({arg['type']!r}, {name})")
    params = ", ".join(required + optional)
    call = f"_call({sym!r}{', ' if call_args else ''}{', '.join(call_args)})"
    lines = [f"def {py_name(sym)}({params}):"]
    if clean_type(fn["ret"]) == "void":
        lines.append(f"    {call}")
        lines.append("    return None")
    else:
        lines.append(f"    return {call}")
    lines.append("")
    lines.append(f"{sym} = {py_name(sym)}")
    return lines


def emit_python() -> str:
    definitions = load_definitions()
    functions = collect_functions(definitions)
    bindings = []
    for fn in functions:
        sym = symbol_name(fn)
        args = ", ".join(py_ctype(arg["type"]) for arg in fn["argsT"])
        bindings.append(f"    {sym!r}: ('imgui_wasm_{sym}', [{args}], {py_ctype(fn['ret'])}),")

    lines = [
        "# This file is generated by imgui-wasm-python/tools/generate_bindings.py.",
        "from __future__ import annotations",
        "",
        "import ctypes",
        "from typing import Optional",
        "",
        "from .core import _load",
        "",
        "",
    ]
    for struct, fields in STRUCTS.items():
        field_expr = ", ".join(f"({name!r}, {py_type})" for name, py_type, _ in fields)
        lines += [
            f"class {struct}(ctypes.Structure):",
            f"    _fields_ = [{field_expr}]",
            "",
            "",
        ]
    lines += [
        "def _b(value: str | bytes | None) -> bytes | None:",
        "    if value is None or isinstance(value, bytes):",
        "        return value",
        "    return value.encode('utf-8')",
        "",
        "",
        "def _coerce(type_name: str, value):",
        "    if type_name in {'const char*', 'char*'}:",
        "        return _b(value)",
        "    return value",
        "",
        "",
        "_BINDINGS = {",
        *bindings,
        "}",
        "",
        "",
        "def _bind() -> ctypes.CDLL:",
        "    lib = _load()",
        "    for _, (export, argtypes, restype) in _BINDINGS.items():",
        "        fn = getattr(lib, export)",
        "        fn.argtypes = argtypes",
        "        fn.restype = restype",
        "    return lib",
        "",
        "",
        "def _call(c_name: str, *args):",
        "    export = _BINDINGS[c_name][0]",
        "    return getattr(_bind(), export)(*args)",
        "",
        "",
    ]

    for fn in functions:
        lines += emit_python_function(fn)
        lines.append("")
        lines.append("")

    exported = sorted(
        set(
            [py_name(symbol_name(fn)) for fn in functions]
            + [symbol_name(fn) for fn in functions]
            + [
                "show_demo_window",
                "begin",
                "text",
                "button",
                "checkbox",
                "slider_float",
                "selectable_bool",
                "selectable_bool_ptr",
            ]
        )
    )
    lines += [
        "def show_demo_window(opened: Optional[bool] = None) -> bool:",
        "    value = ctypes.c_bool(True if opened is None else opened)",
        "    igShowDemoWindow(ctypes.byref(value))",
        "    return bool(value.value)",
        "",
        "",
        "def begin(name: str, opened: Optional[bool] = None, flags: int = 0) -> tuple[bool, bool]:",
        "    value = ctypes.c_bool(True if opened is None else opened)",
        "    visible = bool(igBegin(_b(name), ctypes.byref(value), flags))",
        "    return visible, bool(value.value)",
        "",
        "",
        "def text(value: str) -> None:",
        "    igTextUnformatted(_b(value), None)",
        "",
        "",
        "def button(label: str) -> bool:",
        "    return bool(igButton(_b(label), ImVec2(0.0, 0.0)))",
        "",
        "",
        "def checkbox(label: str, value: bool) -> tuple[bool, bool]:",
        "    c_value = ctypes.c_bool(value)",
        "    changed = bool(igCheckbox(_b(label), ctypes.byref(c_value)))",
        "    return changed, bool(c_value.value)",
        "",
        "",
        "def slider_float(label: str, value: float, min_value: float, max_value: float) -> tuple[bool, float]:",
        "    c_value = ctypes.c_float(value)",
        "    changed = bool(igSliderFloat(_b(label), ctypes.byref(c_value), min_value, max_value, b'%.3f', 0))",
        "    return changed, float(c_value.value)",
        "",
        "",
        "def selectable_bool(label: str, selected: bool = False, flags: int = 0, size: ImVec2 = ImVec2(0.0, 0.0)) -> bool:",
        "    return bool(_call('igSelectable', _b(label), selected, flags, size))",
        "",
        "",
        "def selectable_bool_ptr(label: str, p_selected, flags: int = 0, size: ImVec2 = ImVec2(0.0, 0.0)) -> bool:",
        "    return bool(_call('igSelectableBoolPtr', _b(label), p_selected, flags, size))",
        "",
        "",
        f"__all__ = {exported!r}",
        "",
    ]
    return "\n".join(lines)


def rust_arg(index: int) -> str:
    return f"arg{index}"


def emit_rust() -> str:
    definitions = load_definitions()
    functions = collect_functions(definitions)
    lines = [
        "// This file is generated by imgui-wasm-python/tools/generate_bindings.py.",
        "#![allow(non_snake_case)]",
        "#![allow(unused_imports)]",
        "use std::ffi::c_void;",
        "use std::os::raw::{c_char, c_double, c_int, c_uchar, c_uint, c_ushort};",
        "",
    ]
    for struct, fields in STRUCTS.items():
        lines += [
            "#[repr(C)]",
            "#[derive(Clone, Copy)]",
            f"pub struct {struct} {{",
            *[f"    pub {name}: {rust_ty}," for name, _, rust_ty in fields],
            "}",
            "",
        ]
    lines.append('extern "C" {')
    for fn in functions:
        sym = symbol_name(fn)
        params = ", ".join(f"{rust_arg(i)}: {rust_type(arg['type'])}" for i, arg in enumerate(fn["argsT"]))
        lines.append(f"    #[link_name = \"{sym}\"]")
        lines.append(f"    fn c_{sym}({params}) -> {rust_type(fn['ret'])};")
    lines += ["}", ""]

    for fn in functions:
        sym = symbol_name(fn)
        params = ", ".join(f"{rust_arg(i)}: {rust_type(arg['type'])}" for i, arg in enumerate(fn["argsT"]))
        call_args = ", ".join(rust_arg(i) for i, _ in enumerate(fn["argsT"]))
        ret = rust_type(fn["ret"])
        lines.append("#[no_mangle]")
        lines.append(f"pub unsafe extern \"C\" fn imgui_wasm_{sym}({params}) -> {ret} {{")
        if ret == "()":
            lines.append(f"    c_{sym}({call_args});")
        else:
            lines.append(f"    c_{sym}({call_args})")
        lines.append("}")
        lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Call-stream emitters. Each consumes the single schema produced by
# build_callstream_schema(); all consumers stay in lockstep because they share
# that schema (regenerated together by this script).
#
# Wire encoding of one opcoded call (all little-endian):
#     opcode(u16)
#     for each arg, in declaration order, by category:
#         scalar      -> value, width bytes
#         enum        -> u32 value
#         string      -> u32 interned string id (table sent once per session)
#         vec2        -> 2 x f32
#         vec4        -> 4 x f32
#         floatarr_in -> u32 count, then count x f32
#         strarr_in   -> u32 count, then count x u32 string ids
#         floatarr    -> N x f32 (N from schema; server-echoed current values)
#         ptr_out_scalar -> 1 scalar (server-authoritative current value)
#         ptr_buf     -> u32 len, then len bytes (server-echoed current content)
#
# A frame is: header(6 f32) + frame_id(u32) + call_count(u32) + calls.
# String tables travel in a separate 0x09 message (or inlined in the first
# frame that references a new string); see callstream_protocol.rs / imgui_wasm.js.

_HEADER = "// This file is generated by imgui-wasm-python/tools/generate_bindings.py. Do not edit."


def emit_callstream() -> None:
    """Generate every call-stream artifact from one schema pass."""
    definitions = load_definitions()
    functions = collect_callstream_functions(definitions)
    schema = build_callstream_schema(functions)

    # Ensure output dirs exist.
    for out in (OUT_CS_SCHEMA, OUT_CS_OPCODES_H, OUT_CS_PATCH_SPECS,
                OUT_CS_REPLAY_SWITCH, OUT_CS_CAPTURE_SHIMS, OUT_CS_CAPTURE_HPP):
        out.parent.mkdir(parents=True, exist_ok=True)

    OUT_CS_SCHEMA.write_text(json.dumps(schema, indent=2) + "\n")
    OUT_CS_OPCODES_H.write_text(emit_opcodes_h(schema))
    OUT_CS_PATCH_SPECS.write_text(json.dumps(emit_patch_specs(schema), indent=2) + "\n")
    OUT_CS_REPLAY_SWITCH.write_text(emit_replay_switch_cpp(schema))
    OUT_CS_CAPTURE_SHIMS.write_text(emit_capture_shims_h(schema))
    OUT_CS_CAPTURE_HPP.write_text(emit_capture_hpp(schema))


def op_const(opname: str) -> str:
    # igButton -> OP_IG_BUTTON; igCombo_Str_arr -> OP_IG_COMBO_STR_ARR;
    # igBeginChild_ID -> OP_IG_BEGIN_CHILD_ID (preserve existing _ boundaries).
    body = opname[2:] if opname.startswith("ig") else opname
    # The body mixes CamelCase with _Suffix tokens. Split on existing _ first,
    # then split each token's leading camel hump (Button, BeginChild) into
    # words, but keep all-caps runs (ID, U32, Str) intact.
    parts = []
    for token in body.split("_"):
        if not token:
            continue
        # Split camelCase but preserve runs of uppercase (ID, U32, Str, IntPtr).
        words = re.findall(r"[A-Z]+(?![a-z])|[A-Z][a-z]+|[0-9]+|[a-z]+", token)
        parts.extend(words)
    return "OP_IG_" + "_".join(p.upper() for p in parts)


def emit_opcodes_h(schema: dict) -> str:
    lines = [
        _HEADER,
        "// Stable opcode assignment for the ImGuiWasm call-stream transport.",
        "// Opcodes are index+1 into the sorted opname set; 0 is reserved.",
        "// Regenerate with: python3 imgui-wasm-python/tools/generate_bindings.py",
        "#pragma once",
        "",
        "#define OP_CALLSTREAM_NULL 0",
        "",
    ]
    for op in schema["ops"]:
        lines.append(f"#define {op_const(op['opname'])} {op['opcode']}u")
    lines.append("")
    lines.append(f"// Total call-stream opcodes: {len(schema['ops'])}")
    lines.append("")
    return "\n".join(lines)


def _cs_rust_type(cat: str, width: int | None = None, flen: int | None = None) -> str:
    """Rust type a capture shim receives the arg as (matches generated_imgui_api
    Rust types, so shims can sit at the same ABI seam)."""
    if cat == "scalar":
        return {1: "i8" if width else "u8", 2: "i16", 4: "i32", 8: "i64"}.get(width, "u8")
    return {
        "enum": "i32",
        "vec2": "ImVec2",
        "vec4": "ImVec4",
    }.get(cat, "()")


def emit_patch_specs(schema: dict) -> dict:
    """Per-function spec consumed by build.rs to patch Dear ImGui:
      - rename the definition `ImGui::<funcname>` -> `ImGui::<funcname>__raw`
        at its definition line (op['location']), so the right overload is hit
        when several overloads share a funcname (Combo, BeginChild, ...).
      - inject an inline capture wrapper `ImGui::<funcname>(<args>)` into a
        header included by all imgui sources, forwarding to `__raw`.
    The wrapper signature must match ImGui's public overload exactly (same
    arg types + defaults), so build.rs synthesizes the wrapper text from this
    spec plus the binding argument/default metadata; defaults are preserved."""
    funcs = []
    for op in schema["ops"]:
        funcname = op["funcname"]
        funcs.append({
            "opname": op["opname"],
            "opcode": op["opcode"],
            "const": op_const(op["opname"]),
            "funcname": funcname,           # real C++ method, e.g. "Combo"
            "raw_name": f"{funcname}__raw",
            "location": op["location"],     # "imgui_widgets.cpp:762"
            "args": op["args"],
            "defaults": op["defaults"],
            "ret": op["ret"],
        })
    return {"functions": funcs, "count": len(funcs)}


def _arg_local_name(arg: dict, index: int) -> str:
    n = re.sub(r"\W+", "_", arg.get("name", "")).strip("_")
    return f"a{index}" if not n else n


def op_args_ctype(arg: dict) -> str:
    """C++ type to cast a deserialized enum/scalar back into for the replay
    call. Uses the cleaned binding type verbatim (it is a valid C++ typename)."""
    return clean_type(arg["type"])


def cpp_arg_decl(arg: dict, fn_defaults: dict) -> tuple[str, str]:
    """Return (declaration_text, forwarded_expr) for one arg in a host-facing
    C++ capture wrapper. The declaration matches ImGui's real C++ signature
    (so host code compiles unchanged); forwarded_expr is what the wrapper
    passes to ImGui::<Widget>."""
    name = arg["name"]
    t = clean_type(arg["type"])
    cat = arg["cat"]

    # Map binding arg type -> C++ declaration type. ImGui uses `const ImVec2&`
    # for by-value structs; generators report those as values.
    if cat in ("vec2", "vec4"):
        # t may already be "const ImVec2" / "ImVec2"; normalize to one const.
        base = t.replace("const ", "").strip()
        decl_type = f"const {base}&"
        fwd = name
    elif cat == "scalar":
        decl_type = arg["cpp_type"]
        fwd = name
    elif cat == "enum":
        decl_type = t  # ImGuiCol, ImGuiWindowFlags, etc.
        fwd = name
    elif cat == "string":
        decl_type = "const char*"
        fwd = name
    elif cat == "floatarr":
        # float[4] metadata -> `float*` C++ (ImGui takes float col[N] which
        # decays to float*).
        decl_type = "float*"
        fwd = name
    elif cat == "ptr_out_scalar":
        bt = arg["type"].rstrip("*")
        decl_type = f"{bt}*"
        fwd = name
    elif cat == "ptr_buf":
        decl_type = "char*"
        fwd = name
    elif cat == "floatarr_in":
        decl_type = "const float*"
        fwd = name
    elif cat == "strarr_in":
        decl_type = "const char* const*"
        fwd = name
    else:
        decl_type = t
        fwd = name

    decl = decl_type + f" {name}"
    if name in fn_defaults:
        d = fn_defaults[name]
        # Translate binding default expressions to C++. ImVec2(0,0) passes through.
        cpp_default = {
            "NULL": "nullptr",
            "((void*)0)": "nullptr",
        }.get(d, d)
        decl += f" = {cpp_default}"
    return decl, fwd


def emit_replay_switch_cpp(schema: dict) -> str:
    """The browser-side replay dispatch: one case per opcode, calling the
    unmodified ImGui C++ API after deserializing args from the byte stream.
    This file is compiled into the WASM replay twin (Phase 3).

    For each arg it reads from a Cursor over the frame bytes; output pointers
    point at scratch storage that is discarded (server-authoritative echo)."""
    lines = [_HEADER, """// Replay dispatch for the ImGuiWasm call-stream transport.
// Generated; one case per opcode. Compiled into the browser WASM ImGui twin.
#include "imgui.h"
#include "imgui_wasm_opcodes.h"

namespace ImGuiWasm { namespace replay {

// Cursor over the frame byte buffer. All multi-byte values are little-endian.
struct Cursor {
    const unsigned char* p;
    const unsigned char* end;
    bool overflow = false;

    inline unsigned u8()  { if (p+1>end){overflow=true;return 0;} unsigned v=p[0]; p+=1; return v; }
    inline unsigned u16() { if (p+2>end){overflow=true;return 0;} unsigned v=(unsigned)p[0]|((unsigned)p[1]<<8); p+=2; return v; }
    inline unsigned u32() { if (p+4>end){overflow=true;return 0;} unsigned v=(unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24); p+=4; return v; }
    inline unsigned long long u64() { if (p+8>end){overflow=true;return 0;} unsigned long long v=0; for(int i=0;i<8;i++) v|=((unsigned long long)p[i])<<(8*i); p+=8; return v; }
    inline float f32() { unsigned v=u32(); float f; __builtin_memcpy(&f,&v,4); return f; }
    inline double f64() { unsigned long long v=u64(); double d; __builtin_memcpy(&d,&v,8); return d; }
    // Copy `n` bytes into out; does NOT advance past bounds errors.
    inline void bytes(unsigned char* out, unsigned n) {
        if (p+n>end){overflow=true; return;}
        __builtin_memcpy(out,p,n); p+=n;
    }
};

// Resolve an interned string id to a stable const char*. Implemented by the
// web backend (Phase 3), which owns the per-session string table.
const char* lookup_string(unsigned id);

}} // namespace ImGuiWasm::replay
""", "", "// Deserialize every call in a frame body (after the 6-f32 header +",
 "// frame_id u32 + call_count u32 have been consumed by the caller).",
 "// Returns when call_count calls have been dispatched or the cursor overflows.",
 "extern \"C\" void imweb_replay_calls(const unsigned char* body, unsigned body_len, unsigned call_count) {",
 "    using namespace ImGuiWasm::replay;",
 "    Cursor c{body, body+body_len, false};",
 "    // The byte stream is self-framing (each call's arg widths are determined",
 "    // by its opcode schema), so we decode until the buffer is exhausted.",
 "    // call_count is an optional hint (may be 0 = unknown); the cursor's",
 "    // position and overflow flag are the real termination condition.",
 "    (void)call_count;",
 "    while (c.p < c.end && !c.overflow) {",
 "        unsigned opcode = c.u16();",
 "        switch (opcode) {",
    ]

    for op in schema["ops"]:
        const = op_const(op["opname"])
        funcname = op["funcname"]  # real C++ method, e.g. "Combo"
        # Emit each arg's read IN DECLARATION ORDER (the capture side encodes
        # in the same order; grouped reads would desync the byte stream).
        body = []
        call_args = []
        for i, arg in enumerate(op["args"]):
            name = _arg_local_name(arg, i)
            cat = arg["cat"]
            if cat == "scalar":
                w = arg["width"]
                bits = w * 8
                ctype = arg["cpp_type"]
                # The scratch int must match the width (8-byte scalars need a
                # 64-bit scratch, else the memcpy reads past it).
                scratch = {1: "unsigned char", 2: "unsigned short",
                           4: "unsigned", 8: "unsigned long long"}[w]
                body.append(f"            {ctype} {name}; {{ {scratch} _v=c.u{bits}(); __builtin_memcpy(&{name},&_v,sizeof({name})); }}")
                call_args.append(name)
            elif cat == "enum":
                body.append(f"            int {name}_v = (int)c.u32();")
                call_args.append(f"({op_args_ctype(arg)}){name}_v")
            elif cat == "string":
                body.append(f"            const char* {name} = ImGuiWasm::replay::lookup_string(c.u32());")
                call_args.append(name)
            elif cat == "vec2":
                body.append(f"            float {name}_x=c.f32(), {name}_y=c.f32();")
                call_args.append(f"ImVec2({name}_x,{name}_y)")
            elif cat == "vec4":
                body.append(f"            float {name}_x=c.f32(),{name}_y=c.f32(),{name}_z=c.f32(),{name}_w=c.f32();")
                call_args.append(f"ImVec4({name}_x,{name}_y,{name}_z,{name}_w)")
            elif cat == "floatarr":
                n = arg["len"]
                body.append(f"            float {name}_s[{n}]; for(int _i=0;_i<{n};++_i) {name}_s[_i]=c.f32();")
                call_args.append(f"{name}_s")
            elif cat == "ptr_out_scalar":
                bt = arg["type"].rstrip("*")
                w = arg["width"]
                bits = w * 8
                scratch = {1: "unsigned char", 2: "unsigned short",
                           4: "unsigned", 8: "unsigned long long"}[w]
                body.append(f"            {bt} {name}_s; {{ {scratch} _v=c.u{bits}(); __builtin_memcpy(&{name}_s,&_v,sizeof({name}_s)); }}")
                call_args.append(f"&{name}_s")
            elif cat == "ptr_buf":
                body.append(f"            static char {name}_s[1024]; {{ unsigned _n=c.u32(); if(_n>=sizeof({name}_s))_n=sizeof({name}_s)-1; c.bytes((unsigned char*){name}_s,_n); {name}_s[_n]=0; }}")
                call_args.append(f"{name}_s")
            elif cat == "floatarr_in":
                body.append(f"            static float {name}_s[4096]; {{ unsigned _n=c.u32(); if(_n>4096)_n=4096; for(unsigned _i=0;_i<_n;++_i){name}_s[_i]=c.f32(); }}")
                call_args.append(f"{name}_s")
            elif cat == "strarr_in":
                body.append(f"            static const char* {name}_s[256]; {{ unsigned _n=c.u32(); if(_n>256)_n=256; for(unsigned _i=0;_i<_n;++_i){name}_s[_i]=ImGuiWasm::replay::lookup_string(c.u32()); }}")
                call_args.append(f"{name}_s")
        ret = op["ret"]
        ret_prefix = "" if ret == "void" else "(void)"
        lines.append(f"        case {const}: {{")
        lines.extend(body)
        lines.append(f"            {ret_prefix}ImGui::{funcname}({', '.join(call_args)});")
        lines.append("            break;")
        lines.append("        }")

    lines += [
        "            default:",
        "                // Unknown opcode: cannot resync a byte stream. Abort.",
        "                return;",
        "        }",
        "    }",
        "}",
        "",
    ]
    return "\n".join(lines)


def emit_capture_shims_h(schema: dict) -> str:
    """C++ capture shim declarations. The bodies are generated in the same file
    so build.rs can inject them into imgui.h after renaming the real impls.

    Each shim is an inline `ImGui::<cpp>(...)` that records its args into the
    thread-local capture buffer (when enabled) then forwards to `<cpp>__raw`.
    The capture macros live in imgui_wasm_capture.h (Phase 1).

    NOTE: this file documents the intended patch shape. build.rs consumes
    patch_specs.json and synthesizes the per-function wrapper text directly
    (one inline per function, with defaults from metadata), because the wrapper
    signature must exactly match ImGui's public signature (incl. defaults) so
    host code compiles unchanged. This header only provides the macro layer."""
    return "\n".join([
        _HEADER,
        """//
// Call-stream capture shims. These are NOT standalone declarations; they
// document the patch shape applied to imgui.h by build.rs (Phase 1):
//
//   inline <ret> ImGui::<Cpp>(<args with defaults>) {
//       IMGUI_WASM_CAPTURE_BEGIN(OP_IG_<OPNAME>);
//       <one IMGUI_WASM_CAPTURE_ARG_* per arg, by category>
//       IMGUI_WASM_CAPTURE_END();
//       return ImGui::<Cpp>__raw(<forwarded args>);
//   }
//
// The actual per-function wrapper text is generated by build.rs from
// patch_specs.json so that default-argument expressions are preserved.
#pragma once

// Defined in imgui_wasm_capture.h (Phase 1). Re-declared here only to keep the
// generated tree self-describing.
#include "imgui_wasm_capture.h"
""",
        "",
    ])


def _capture_encode_stmt(arg: dict, index: int) -> str:
    """One imgui_wasm_capture_* call that serializes an arg into the thread-local
    capture buffer. Mirrors the wire encoding in replay_switch.cpp exactly."""
    name = arg["name"]
    cat = arg["cat"]
    if cat == "scalar":
        w = arg["width"]
        # Preserve floating-point bit patterns exactly. A numeric cast to an
        # integer changes the value (for example -1.0f became 0xffffffff,
        # which the replay side decoded as NaN), causing browser/server layout
        # and hit-test geometry to diverge.
        if arg["cpp_type"] in ("float", "double"):
            return f"imgui_wasm_capture_ptr((const void*)(&{name}), {w}u);"
        return f"imgui_wasm_capture_u{w*8}((unsigned long long)({name}));"
    if cat == "enum":
        return f"imgui_wasm_capture_u32((unsigned)({name}));"
    if cat == "string":
        return f"imgui_wasm_capture_string({name});"
    if cat == "vec2":
        return f"imgui_wasm_capture_vec2({name}.x, {name}.y);"
    if cat == "vec4":
        return f"imgui_wasm_capture_vec4({name}.x, {name}.y, {name}.z, {name}.w);"
    if cat == "floatarr":
        n = arg["len"]
        return f"imgui_wasm_capture_floats({name}, {n});"
    if cat == "ptr_out_scalar":
        # Serialize the pointee's CURRENT value (server-authoritative echo).
        # Pass the pointer + element width; the capture ABI reads raw bytes
        # (no type punning; preserves float bit patterns).
        bt = arg["type"].rstrip("*")
        w = {"float": 4, "bool": 1, "int": 4, "unsigned int": 4,
             "double": 8, "size_t": 4}[bt]
        return f"imgui_wasm_capture_ptr((const void*)({name}), {w}u);"
    if cat == "ptr_buf":
        return f"imgui_wasm_capture_buf({name} ? {name} : \"\");"
    return None  # floatarr_in / strarr_in handled in the wrapper emitter


def emit_capture_hpp(schema: dict) -> str:
    """The host-facing capture header. Defines `namespace imgui_wasm { ... }` with
    one inline wrapper per opcode. Each wrapper:
      1. records opcode + args into the thread-local capture buffer (via the
         imgui_wasm_capture_* C ABI implemented in capture.rs), then
      2. forwards to the real ImGui::<funcname>.

    This is the single capture point. The public C++ header redirects the
    original ImGui:: namespace spelling to these wrappers, while `using
    namespace ImGui` makes unsupported calls and queries fall through to the
    native API. ImGui's own internal calls are not redirected. Python hosts
    use the ig* ABI, which is captured separately.

    When call-stream transport is disabled at runtime, every capture_* call is
    a cheap atomic-load-and-branch no-op, so this header is safe to include
    unconditionally."""
    lines = [_HEADER, """//
// ImGuiWasm call-stream capture surface (host-facing).
//
// Applications keep using the original ImGui::<Widget> API. The public
// imgui_wasm.hpp header redirects that namespace spelling to this capture
// surface after all library declarations have been parsed.
//
// When call-stream transport is OFF, capture is a no-op and these wrappers
// are zero-cost forwards to ImGui::.
#pragma once

#include "imgui.h"
#include "imgui_wasm_opcodes.h"
#include "imgui_wasm_capture.h"

namespace imgui_wasm {

// Queries and unsupported calls retain their native ImGui implementation.
using namespace ImGui;
"""]

    def needs_count(arg):
        return arg["cat"] in ("floatarr_in", "strarr_in")

    for op in schema["ops"]:
        const = op_const(op["opname"])
        funcname = op["funcname"]
        ret = op["ret"]
        defaults = op["defaults"]

        # Build arg declarations + forwarded exprs.
        decls = []
        fwds = []
        for i, arg in enumerate(op["args"]):
            decl, fwd = cpp_arg_decl(arg, defaults)
            decls.append(decl)
            fwds.append(fwd)
        sig = "    inline " + ("void " if ret == "void" else f"{op['ret']} ") + funcname + "(" + ", ".join(decls) + ")"

        # For floatarr_in/strarr_in, capture uses the sibling count arg.
        # Resolve each such arg to its count arg name.
        count_arg_idx = {}
        for i, arg in enumerate(op["args"]):
            if needs_count(arg):
                # find a sibling scalar arg named *_count or items_count etc.
                cnt = None
                for j, a2 in enumerate(op["args"]):
                    if a2["cat"] == "scalar" and ("count" in a2["name"] or a2["name"] == "n"):
                        cnt = a2["name"]
                        break
                count_arg_idx[i] = cnt or "_count_missing"

        body = []
        body.append(f"        imgui_wasm_capture_begin({const});")
        for i, arg in enumerate(op["args"]):
            cat = arg["cat"]
            name = arg["name"]
            if cat == "floatarr_in":
                # Wire order: count(u32) THEN count*float. Replay reads the same.
                cnt = count_arg_idx.get(i, "n")
                body.append(f"        imgui_wasm_capture_u32((unsigned)({cnt}));")
                body.append(f"        imgui_wasm_capture_floats_n({name}, (unsigned)({cnt}));")
            elif cat == "strarr_in":
                cnt = count_arg_idx.get(i, "n")
                body.append(f"        imgui_wasm_capture_u32((unsigned)({cnt}));")
                body.append(f"        imgui_wasm_capture_strings_n({name}, (unsigned)({cnt}));")
            else:
                stmt = _capture_encode_stmt(arg, i)
                if stmt:
                    body.append(f"        {stmt}")
        body.append(f"        imgui_wasm_capture_end();")

        if ret == "void":
            body.append(f"        ImGui::{funcname}({', '.join(fwds)});")
        else:
            body.append(f"        return ImGui::{funcname}({', '.join(fwds)});")

        lines.append(sig + " {")
        lines.extend(body)
        lines.append("    }")
        lines.append("")

    lines.append("} // namespace imgui_wasm")
    lines.append("")
    lines.append("#ifndef IMGUI_WASM_NO_IMGUI_REDIRECT")
    lines.append("#define ImGui imgui_wasm")
    lines.append("#endif")
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    OUT_PY.write_text(emit_python())
    OUT_RS.write_text(emit_rust())
    emit_callstream()


if __name__ == "__main__":
    main()
