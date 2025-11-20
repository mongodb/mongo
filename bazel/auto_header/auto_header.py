import subprocess
import hashlib
import os
import threading
import time
import json
from collections import defaultdict
from functools import lru_cache
from pathlib import Path
from typing import Dict, List, Set, Tuple
from io import StringIO


SRC_ROOT = Path("src")
AUTO_DIR = ".auto_header"
BUILD_NAME = "BUILD.bazel"

SRC_EXTS_TUPLE = (
    ".cpp",
    ".cc",
    ".cxx",
    ".c",
    ".h",
    ".hpp",
    ".hh",
    ".hxx",
    ".idl",
    ".idl.tpl",
    ".inl",
)
HDR_EXTS_TUPLE = (".h", ".hh", ".hpp", ".hxx", ".inl")
LEFT_PSEUDO_SUFFIX = "_gen"

# Anything produced by protoc/grpc should be provided by proto/grpc rules, not auto_header
PROTO_GEN_SUFFIXES = (".grpc.pb.h", ".pb.h")  # order matters only for readability

SRC_ROOT_POSIX = SRC_ROOT.as_posix()
AUTO_HEADER_PREFIX = f"//{SRC_ROOT_POSIX}/"
VERSION_SALT = "autoheader-v7"  # bump to force regen
MANIFEST_PATH = SRC_ROOT / AUTO_DIR / "last_run.json"

# -------- single-pass file lister (cache) --------
_FILE_LIST_CACHE: list[str] | None = None

# Exact-path remaps for generated, non-IDL headers.
# Key: repo-relative include path as seen on the right side (normalized with /).
# Val: absolute Bazel label of the generator target to depend on.
GEN_HEADER_REMAP = {
    "mongo/config.h": f"//{SRC_ROOT_POSIX}/mongo:mongo_config_header",
}

EXCLUDE_HEADERS = {
    "mongo/platform/windows_basic.h",  # forced via command line; don’t depend on it
    "mongo/scripting/mozjs/freeOpToJSContext.h",  # weird mongo include that lives in 3rd party
    "mongo/scripting/mozjs/mongoErrorReportToString.h",  # weird mongo include that lives in 3rd party
}

# Generated *_cpp (or *_gen) left files that we want to emit regardless of rg.
# Key: left path under repo (e.g. "mongo/util/version_impl.cpp" or "mongo/foo/bar_gen")
# Val: list of right includes under repo ("mongo/...") which will be mapped with _label_for_right()
GEN_LEFT_CPP: dict[str, list[str]] = {
    "mongo/shell/mongo-server.cpp": [
        "mongo/base/string_data.h",
        "mongo/scripting/engine.h",
    ],
    "mongo/shell/mongojs.cpp": [
        "mongo/base/string_data.h",
        "mongo/scripting/engine.h",
    ],
    "mongo/scripting/mozjs/mongohelpers_js.cpp": [
        "mongo/base/string_data.h",
        "mongo/scripting/engine.h",
    ],
    "mongo/db/fts/stop_words_list.cpp": [
        "mongo/db/fts/stop_words_list.h",
    ],
    "mongo/db/fts/unicode/codepoints_casefold.cpp": [
        "mongo/db/fts/unicode/codepoints.h",
    ],
    "mongo/db/fts/unicode/codepoints_delimiter_list.cpp": [
        "mongo/db/fts/unicode/codepoints.h",
    ],
    "mongo/db/fts/unicode/codepoints_diacritic_list.cpp": [
        "mongo/db/fts/unicode/codepoints.h",
    ],
}

# Headers every IDL uses (as repo-relative include paths, e.g. "mongo/.../x.h").
IDL_HEADERS_RIGHTS = [
    "mongo/base/string_data.h",
    "mongo/base/data_range.h",
    "mongo/bson/bsonobj.h",
    "mongo/bson/bsonobjbuilder.h",
    "mongo/bson/simple_bsonobj_comparator.h",
    "mongo/idl/idl_parser.h",
    "mongo/rpc/op_msg.h",
    "mongo/db/auth/validated_tenancy_scope_factory.h",
    "mongo/stdx/unordered_map.h",
    "mongo/util/decimal_counter.h",
    "mongo/util/serialization_context.h",
    "mongo/util/options_parser/option_description.h",
    "mongo/util/options_parser/option_section.h",
    "mongo/util/options_parser/environment.h",
    "mongo/db/feature_flag.h",
    "mongo/db/feature_flag_server_parameter.h",
    "mongo/db/feature_compatibility_version_parser.h",
    "mongo/db/server_parameter.h",
    "mongo/db/server_parameter_with_storage.h",
    "mongo/db/commands.h",
    "mongo/db/query/query_shape/serialization_options.h",
    "mongo/util/overloaded_visitor.h",
    "mongo/util/string_map.h",
    "mongo/db/auth/authorization_contract.h",
    "mongo/idl/command_generic_argument.h",
    "mongo/util/options_parser/startup_option_init.h",
    "mongo/util/options_parser/startup_options.h",
]


def _norm_repo(p: str) -> str:
    return p.replace("\\", "/").lstrip("./")


def _is_proto_generated_header(r: str) -> bool:
    return r.endswith(PROTO_GEN_SUFFIXES)


def _is_excluded_right(r: str) -> bool:
    if r in EXCLUDE_HEADERS:
        return True
    return _is_proto_generated_header(r)


def _files_identical(path: Path, data: bytes, chunk_size: int = 1 << 20) -> bool:
    try:
        st = path.stat()
    except FileNotFoundError:
        return False
    if st.st_size != len(data):
        return False
    mv = memoryview(data)
    with path.open("rb", buffering=0) as f:
        off = 0
        while off < len(data):
            n = min(chunk_size, len(data) - off)
            if f.read(n) != mv[off : off + n]:
                return False
            off += n
    return True


def _idl_gen_header_file_label(repo_rel_idl: str) -> str:
    if repo_rel_idl.endswith(".idl.tpl"):
        base = repo_rel_idl[: -len(".idl.tpl")]
    elif repo_rel_idl.endswith(".idl"):
        base = repo_rel_idl[: -len(".idl")]
    else:
        base = repo_rel_idl
    d, f = base.rsplit("/", 1)
    return f"//{SRC_ROOT_POSIX}/{d}:{f}_gen.h"


def _atomic_write_if_changed(path: Path, text: str, *, fsync_write: bool) -> bool:
    data = text.encode("utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    if _files_identical(path, data):
        return False
    tmp = path.with_suffix(path.suffix + ".tmp")
    with tmp.open("wb", buffering=0) as f:
        f.write(data)
        if fsync_write:
            f.flush()
            os.fsync(f.fileno())
    os.replace(tmp, path)
    return True


@lru_cache(maxsize=2**20)
def _sanitize_label(s: str) -> str:
    out = []
    for i, ch in enumerate(s):
        if ch.isalnum() or ch == "_":
            if i == 0 and ch.isdigit():
                out.append("_")
            out.append(ch)
        else:
            out.append("_")
    return "".join(out) or "_"


@lru_cache(maxsize=2**15)  # 32k entries
def _is_third_party_path(p: str) -> bool:
    p = p.replace("\\", "/")
    # match any path segment "third_party" (not just a substring)
    return (
        "/third_party/" in p
        or p.startswith("third_party/")
        or p.endswith("/third_party")
        or "/src/third_party/" in p
        or p.startswith("src/third_party/")
    )


def _build_left_rights_map(grouped: Dict[str, Dict[str, Set[str]]]) -> Dict[str, Set[str]]:
    out: Dict[str, Set[str]] = {}
    for d, files in grouped.items():
        for f, rights in files.items():
            out[f"{d}/{f}"] = set(r for r in rights if r.startswith("mongo/"))
    return out


@lru_cache(maxsize=2**15)
def _is_header_include_path(p: str) -> bool:
    return p.endswith(HDR_EXTS_TUPLE)


@lru_cache(maxsize=2**15)
def _repo_to_file_label(repo_rel: str) -> str:
    d, f = repo_rel.rsplit("/", 1)
    return f"//{SRC_ROOT_POSIX}/{d}:{f}"


def _dfs_flatten_from_seed(
    seed_repo_path: str, lr_map: Dict[str, Set[str]], visited: Set[str], out_labels: Set[str]
) -> None:
    srp = seed_repo_path.replace("\\", "/")
    if _is_third_party_path(srp) or _is_excluded_right(srp):
        return
    if srp in visited:
        return
    visited.add(srp)

    # Add terminal label(s)
    if _is_header_include_path(srp) or srp.endswith("_gen.h"):
        out_labels.add(_repo_to_file_label(srp))

    if srp.endswith(".idl") or srp.endswith(".idl.tpl"):
        # include the generated header for this IDL
        out_labels.add(_idl_gen_header_file_label(srp))

    # if we hit a *_gen.h, also walk the owning IDL’s imports
    if srp.endswith("_gen.h"):
        idl_owner = _guess_idl_from_gen_header(srp, lr_map)
        if idl_owner:
            _dfs_flatten_from_seed(idl_owner, lr_map, visited, out_labels)

    # Recurse through children (includes/imports) if known
    for child in lr_map.get(srp, ()):
        if _is_excluded_right(child):
            continue
        _dfs_flatten_from_seed(child, lr_map, visited, out_labels)


def _rg_all_files_once(rg_bin: str) -> list[str]:
    """One ripgrep call to list all relevant files under src/mongo/**."""
    global _FILE_LIST_CACHE
    if _FILE_LIST_CACHE is not None:
        return _FILE_LIST_CACHE

    # Combine all globs into one invocation.
    globs = [
        "mongo/**/*.idl",
        "mongo/**/*.idl.tpl",
        "mongo/**/*.{c,cc,cpp,cxx,h,hh,hpp,hxx,inl}",
    ]
    cmd = [
        rg_bin,
        "--files",
        "--no-config",
        "-uu",
        *[x for g in globs for x in ("-g", g)],
        "mongo",
    ]
    out = _run_cmd(cmd)

    # Normalize & filter once.
    files = []
    for p in out:
        rp = _norm_repo(p)
        if not rp.startswith("mongo/"):
            continue
        if _is_third_party_path(rp):
            continue
        files.append(rp)
    _FILE_LIST_CACHE = files
    return files


def _load_manifest_dirs() -> set[str]:
    try:
        with MANIFEST_PATH.open("r", encoding="utf-8") as f:
            obj = json.load(f)
            if isinstance(obj, dict) and isinstance(obj.get("dirs"), list):
                # ensure normalized 'mongo/...'
                return {d for d in obj["dirs"] if isinstance(d, str)}
    except FileNotFoundError:
        pass
    except Exception:
        pass
    return set()


def _store_manifest_dirs(dirs: set[str]) -> None:
    MANIFEST_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = MANIFEST_PATH.with_suffix(".tmp")
    with tmp.open("w", encoding="utf-8") as f:
        json.dump({"dirs": sorted(dirs)}, f, separators=(",", ":"))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, MANIFEST_PATH)


def _cleanup_from_manifest(prev_dirs: set[str], curr_dirs: set[str]) -> int:
    """Delete stale .auto_header/BUILD.bazel for dirs that disappeared."""
    removed = 0
    stale = prev_dirs - curr_dirs
    # Fast path: string joins to avoid Path overhead in tight loop
    root = SRC_ROOT.as_posix()  # "src"
    ah = AUTO_DIR  # ".auto_header"
    bn = BUILD_NAME  # "BUILD.bazel"
    for d in stale:
        build_path = f"{root}/{d}/{ah}/{bn}"
        try:
            os.remove(build_path)
            removed += 1
        except FileNotFoundError:
            pass
        # best-effort: remove empty .auto_header dir
        ah_dir = f"{root}/{d}/{ah}"
        try:
            if not any(os.scandir(ah_dir)):
                os.rmdir(ah_dir)
        except Exception:
            pass
    return removed


def _compute_flat_idl_from_lr_map(lr_map: Dict[str, Set[str]], seeds: List[str]) -> Set[str]:
    out: Set[str] = set()
    vis: Set[str] = set()
    for seed in seeds:
        _dfs_flatten_from_seed(seed, lr_map, vis, out)
    return {lab for lab in out if "third_party" not in lab.split("/")}


def _inject_flat_group(grouped: Dict[str, Dict[str, Set[str]]], flat_labels: Set[str]) -> None:
    # Store as a synthetic "left" whose rights are already absolute labels.
    # We'll special-case its rendering to dump labels verbatim.
    grouped.setdefault("mongo", {}).setdefault("idl_headers_flat", set()).update(flat_labels)


def augment_with_idl_placeholders(
    grouped: dict[str, dict[str, set[str]]], idl_paths: list[str]
) -> None:
    """
    Ensure each IDL gets a left-side entry (=> <idl_basename>_gen) even if it
    had zero imports/includes. Adds: grouped[dir][file] = set() when missing.
    """
    for p in idl_paths:
        if _is_third_party_path(p):
            continue
        d, sep, f = p.rpartition("/")
        if not sep:
            continue
        grouped.setdefault(d, {}).setdefault(f, set())


def augment_with_generated_left(
    grouped: Dict[str, Dict[str, Set[str]]], gen_map: Dict[str, List[str]]
) -> None:
    """
    For each generated left entry, ensure a filegroup exists and add the
    configured rights. Skips third_party automatically.
    """
    for left, rights in gen_map.items():
        if not left.startswith("mongo/"):
            continue
        if "third_party/" in left or left.startswith("third_party/"):
            continue

        d, sep, f = left.rpartition("/")
        if not sep:
            continue
        dst = grouped.setdefault(d, {}).setdefault(f, set())
        for r in rights:
            if r and r.startswith("mongo/"):
                dst.add(r)


def augment_with_source_placeholders(
    grouped: Dict[str, Dict[str, Set[str]]], src_paths: List[str]
) -> None:
    """Seed a filegroup for every left source/header even if it has 0 includes."""
    for p in src_paths:
        if _is_third_party_path(p):
            continue
        d, sep, f = p.rpartition("/")
        if not sep:
            continue
        grouped.setdefault(d, {}).setdefault(f, set())


def _render_vis_list(v: list[str]) -> str:
    return ", ".join(f'"{x}"' for x in v)


def list_all_idl_paths_py(rg_bin: str) -> list[str]:
    files = _rg_all_files_once(rg_bin)
    # Keep both .idl and .idl.tpl
    return [f for f in files if f.endswith(".idl") or f.endswith(".idl.tpl")]


def list_all_left_sources_py(rg_bin: str) -> list[str]:
    files = _rg_all_files_once(rg_bin)
    exts = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl")
    return [f for f in files if f.endswith(exts)]


@lru_cache(maxsize=2**15)  # 32k entries
def module_visibility_for_build_dir(src_dir: str) -> list[str]:
    parts = src_dir.replace("\\", "/").split("/")
    # expect starts with "mongo"
    if len(parts) >= 5 and parts[0] == "mongo" and parts[1] == "db" and parts[2] == "modules":
        module_name = parts[3]
        if module_name == "enterprise":
            return [
                "//src/mongo/db/modules/enterprise:__subpackages__",
                "//src/mongo/db/modules/atlas:__subpackages__",
            ]
        return [f"//src/mongo/db/modules/{module_name}:__subpackages__"]
    return ["//visibility:public"]


@lru_cache(maxsize=2**15)  # 32k entries
def _filegroup_name_for_left(left_file: str) -> str:
    # .idl.tpl -> <base>_gen
    fname = left_file.split("/")[-1]

    if left_file.endswith(".idl.tpl"):
        base = left_file[: -len(".idl.tpl")]
        return _sanitize_label(base + "_gen")
    if left_file.endswith(".tpl.cpp"):
        base = left_file[: -len(".tpl.cpp")]
        return _sanitize_label(base + "_cpp")
    if left_file.endswith(".tpl.h"):
        base = left_file[: -len(".tpl.h")]
        return _sanitize_label(base + "_h")
    # .idl -> <base>_gen
    if left_file.endswith(".idl"):
        base = left_file[: -len(".idl")]
        return _sanitize_label(base + "_gen")
    # *_gen stays as-is
    if left_file.endswith(LEFT_PSEUDO_SUFFIX):
        return _sanitize_label(left_file)

    # keep extension-specific suffix (apple.hpp -> apple_hpp)
    for ext in HDR_EXTS_TUPLE:
        if fname.endswith(ext):
            base = fname[: -len(ext)]
            return _sanitize_label(base + "_" + ext[1:])

    return _sanitize_label(left_file)


def _owning_labels_for_left(src_dir: str, left_file: str) -> list[str]:
    """
    Labels that should be included in the filegroup for `left_file` itself:
      - *.idl / *.idl.tpl  -> //pkg:<base>_gen.h   (own the generated header)
      - *.tpl.h            -> //pkg:<base>.h      (own the rendered header)
      - *.tpl.cpp          -> //pkg:<base>.cpp    (own the rendered source)
      - plain headers      -> //pkg:<left_file>   (own the header)
      - otherwise          -> []
    A manual override map can pin special cases to rules, if needed.
    """

    pkg = f"//{SRC_ROOT_POSIX}/{src_dir}"

    # IDL/IDL.TPL: include generated header
    if left_file.endswith(".idl.tpl"):
        base = left_file[: -len(".idl.tpl")]
        return [f"{pkg}:{base}_gen.h"]
    if left_file.endswith(".idl"):
        base = left_file[: -len(".idl")]
        return [f"{pkg}:{base}_gen.h"]

    # Templates → output filename in same pkg
    for tpl_ext, out_ext in {".tpl.h": ".h"}.items():
        if left_file.endswith(tpl_ext):
            phys = left_file[: -len(tpl_ext)] + out_ext
            return [f"{pkg}:{phys}"]

    # Plain headers own themselves
    if any(left_file.endswith(ext) for ext in HDR_EXTS_TUPLE):
        return [f"{pkg}:{left_file}"]

    return []


@lru_cache(maxsize=2**15)  # 32k entries
def _label_for_right(right_path: str) -> str:
    if _is_excluded_right(right_path):
        return ""
    r_dir, _, fname = right_path.rpartition("/")

    if _is_third_party_path(right_path):
        return ""

    remap = GEN_HEADER_REMAP.get(right_path)
    if remap:
        return remap

    # *_gen.h → *_gen
    if fname.endswith("_gen.h"):
        base = fname[: -len(".h")]
        return f"{AUTO_HEADER_PREFIX}{r_dir}/{AUTO_DIR}:{_sanitize_label(base)}"

    # .idl.tpl / .idl → *_gen
    if fname.endswith(".idl.tpl"):
        base = fname[: -len(".idl.tpl")]
        return f"{AUTO_HEADER_PREFIX}{r_dir}/{AUTO_DIR}:{_sanitize_label(base + '_gen')}"
    if fname.endswith(".idl"):
        base = fname[: -len(".idl")]
        return f"{AUTO_HEADER_PREFIX}{r_dir}/{AUTO_DIR}:{_sanitize_label(base + '_gen')}"

    # base + "_" + ext-without-dot (so .hpp → _hpp, .h → _h)
    for ext in HDR_EXTS_TUPLE:
        if fname.endswith(ext):
            base = fname[: -len(ext)]
            suf = "_" + ext[1:]  # strip the leading dot
            target = _sanitize_label(base + suf)
            return f"{AUTO_HEADER_PREFIX}{r_dir}/{AUTO_DIR}:{target}"

    # already *_gen token
    if fname.endswith("_gen"):
        return f"{AUTO_HEADER_PREFIX}{r_dir}/{AUTO_DIR}:{_sanitize_label(fname)}"

    return ""


# ---------- Digest-based skip ----------
HEADER_TPL = """# DIGEST:{digest}
# AUTO-GENERATED. DO NOT EDIT.
# Package: //{pkg}
# Generated from ripgrep scan.

package(default_visibility = [{vis_list}])

"""


def _dir_digest(src_dir: str, files_map: Dict[str, Set[str]], *, visibility: str) -> str:
    lines = []
    lines.append(f"{VERSION_SALT}|{visibility}|{src_dir}\n")
    for left in sorted(files_map):
        rights = ",".join(sorted(files_map[left]))
        lines.append(f"{left}:{rights}\n")
    data = "".join(lines).encode()
    return hashlib.sha1(data).hexdigest()[:16]


def _read_existing_digest(path: Path) -> str:
    try:
        with path.open("r", encoding="utf-8") as f:
            for _ in range(5):  # scan first few lines defensively
                line = f.readline()
                if not line:
                    break
                if line.startswith("# DIGEST:"):
                    return line.split(":", 1)[1].strip()
    except FileNotFoundError:
        pass
    return ""


def _build_content_for_dir(
    src_dir: str, file_to_rights: Dict[str, Set[str]], *, visibility: list[str], digest: str
) -> str:
    pkg = f"{SRC_ROOT_POSIX}/{src_dir}/{AUTO_DIR}"
    buf = StringIO()
    write = buf.write
    write(HEADER_TPL.format(pkg=pkg, vis_list=_render_vis_list(visibility), digest=digest))

    for left_file in sorted(file_to_rights.keys()):
        is_flat = src_dir == "mongo" and left_file == "idl_headers_flat"
        name = _filegroup_name_for_left(left_file)
        this_lbl = f"//{SRC_ROOT_POSIX}/{src_dir}/{AUTO_DIR}:{name}"
        emit_name = "idl_headers" if is_flat else name

        write("filegroup(\n")
        write(f'    name = "{emit_name}",\n')

        seen: Set[str] = set()
        labels: List[str] = []

        if not is_flat:
            for own in _owning_labels_for_left(src_dir, left_file):
                if own and own != this_lbl and own not in seen:
                    labels.append(own)
                    seen.add(own)

        if left_file.endswith((".idl", ".idl.tpl")) or left_file.endswith("_gen"):
            central = f"//{SRC_ROOT_POSIX}/mongo/{AUTO_DIR}:idl_headers"
            if central != this_lbl and central not in seen:
                labels.append(central)
                seen.add(central)

        map_right = (lambda r: r) if is_flat else _label_for_right
        for r in sorted(file_to_rights[left_file]):
            lab = map_right(r)
            if lab and lab != this_lbl and lab not in seen:
                labels.append(lab)
                seen.add(lab)

        if not labels:
            labels.append("//bazel/auto_header:_ah_placeholder.h")

        write("    srcs = [\n")
        for lab in labels:
            write(f'        "{lab}",\n')
        write("    ],\n")
        write(")\n\n")

    return buf.getvalue()


def _guess_idl_from_gen_header(gen_header_repo: str, lr_map: Dict[str, Set[str]]) -> str | None:
    if not gen_header_repo.endswith("_gen.h"):
        return None
    base = gen_header_repo[: -len("_gen.h")]
    cand_idl = base + ".idl"
    cand_tpl = base + ".idl.tpl"
    if cand_idl in lr_map:
        return cand_idl
    if cand_tpl in lr_map:
        return cand_tpl
    return None


def _write_build_for_group(
    src_dir: str, files_map: Dict[str, Set[str]], *, fsync_write: bool
) -> Tuple[str, bool]:
    out_dir = SRC_ROOT / src_dir / AUTO_DIR
    out_path = out_dir / BUILD_NAME
    visibility = module_visibility_for_build_dir(src_dir)
    new_digest = _dir_digest(src_dir, files_map, visibility=_render_vis_list(visibility))
    old_digest = _read_existing_digest(out_path)
    if new_digest == old_digest:
        return (out_path.as_posix(), False)
    content = _build_content_for_dir(src_dir, files_map, visibility=visibility, digest=new_digest)
    changed = _atomic_write_if_changed(out_path, content, fsync_write=fsync_write)
    return (out_path.as_posix(), changed)


def _run_cmd(cmd: list[str]) -> list[str]:
    env = os.environ.copy()
    env.setdefault("LC_ALL", "C")
    env.setdefault("LANG", "C")
    res = subprocess.run(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, cwd="src", env=env
    )
    if res.returncode not in (0, 1):
        raise RuntimeError(f"Command failed ({res.returncode}): {' '.join(cmd)}\n{res.stderr}")
    return res.stdout.splitlines()


def collect_left_right_pairs(rg_bin: str) -> list[tuple[str, str]]:
    cmd_src = [
        rg_bin,
        "-PH",
        "--no-line-number",
        "--color=never",
        "--no-config",
        "-o",
        r'^\s*#\s*include\s*[<"](?P<p>mongo/[^\s">]+)[">]',
        "-g",
        "mongo/**/*.{c,cc,cpp,cxx,h,hh,hpp,inl}",
        "mongo",
        "--replace",
        r"$p",
    ]
    cmd_idl = [
        rg_bin,
        "-PH",
        "--no-line-number",
        "--color=never",
        "--no-config",
        "-o",
        r'^\s*-\s*"?(mongo/[^\s">]+)"?',
        "-g",
        "mongo/**/*.idl",
        "-g",
        "mongo/**/*.idl.tpl",
        ".",
        "--replace",
        r"$1",
    ]

    out1 = _run_cmd(cmd_src)
    out2 = _run_cmd(cmd_idl)

    def _norm_fast(s: str) -> str:
        # avoid hot replace/lstrip if not needed
        if "\\" not in s and not s.startswith("./"):
            return s
        if "\\" in s:
            s = s.replace("\\", "/")
        if s.startswith("./"):
            s = s[2:]
        return s

    merged: set[tuple[str, str]] = set()

    def _ingest(lines: list[str]) -> None:
        for line in lines:
            if not line:
                continue
            left, sep, right = line.partition(":")
            if not sep:
                continue
            left = _norm_fast(left)
            right = _norm_fast(right)
            # early drop: only mongo/, not third_party
            if not right.startswith("mongo/"):
                continue
            if (
                "/third_party/" in left
                or "/third_party/" in right
                or left.startswith("third_party/")
                or right.startswith("third_party/")
            ):
                continue
            merged.add((left, right))

    _ingest(out1)
    _ingest(out2)
    return list(merged)


def parse_left_right_pairs(pairs: list[tuple[str, str]]) -> Dict[str, Dict[str, Set[str]]]:
    by_srcdir = defaultdict(lambda: defaultdict(set))
    add = set.add
    src_exts = SRC_EXTS_TUPLE
    for left, right in pairs:
        left_dir, sep, left_file = left.rpartition("/")
        if not sep:
            continue
        if (not left_file.endswith(src_exts)) and (not left_file.endswith(LEFT_PSEUDO_SUFFIX)):
            continue
        if "/third_party/" in left_dir or left_dir.startswith("third_party/"):
            continue
        add(by_srcdir[left_dir].setdefault(left_file, set()), right)
    return by_srcdir


def _env_bool(var: str, default: bool) -> bool:
    v = os.environ.get(var)
    if v is None:
        return default
    return v not in ("0", "false", "False", "no", "No", "")


def gen_auto_headers(repo_root: Path) -> Dict[str, object]:
    """
    Runs the header generation once and returns a result dict:
      { ok: bool, err: str|None, wrote: int, skipped: int, t_ms: float }
    """
    t0 = time.perf_counter()
    cwd0 = Path.cwd()
    res = {"ok": True, "err": None, "wrote": 0, "skipped": 0, "t_ms": 0.0}

    try:
        from bazel.auto_header.ensure_fd import ensure_rg
    except Exception as e:
        raise RuntimeError("Failed to import ensure_rg (ripgrep bootstrap).") from e

    rg_bin = ensure_rg()
    no_fsync = _env_bool("AUTOHEADER_NO_FSYNC", True)  # skip fsync by default

    try:
        os.chdir(repo_root)

        # load previous
        prev_dirs = _load_manifest_dirs()

        pairs = collect_left_right_pairs(rg_bin)
        grouped = parse_left_right_pairs(pairs)

        # Ensure *_gen targets exist for IDLs with no refs
        idl_paths = list_all_idl_paths_py(rg_bin)
        augment_with_idl_placeholders(grouped, idl_paths)

        # Ensure every .c/.cc/.cpp/.cxx/.h/.hh/.hpp/.hxx gets a filegroup even with 0 includes
        src_paths = list_all_left_sources_py(rg_bin)
        augment_with_source_placeholders(grouped, src_paths)

        augment_with_generated_left(grouped, GEN_LEFT_CPP)

        lr_map = _build_left_rights_map(grouped)  # still needed once
        flat = _compute_flat_idl_from_lr_map(lr_map, IDL_HEADERS_RIGHTS)
        _inject_flat_group(grouped, flat)
        curr_dirs = set(grouped.keys())

        # manifest-based cleanup (fast)
        cleaned = _cleanup_from_manifest(prev_dirs, curr_dirs)

        # 3) Emit per-dir BUILD files (sequential by default; see env knob above)
        wrote = 0
        skipped = 0

        for src_dir, files_map in grouped.items():
            if _is_third_party_path(src_dir):
                continue
            _, changed = _write_build_for_group(  # type: ignore[attr-defined]
                src_dir,
                files_map,
                fsync_write=not no_fsync,
            )
            wrote += int(changed)
            skipped += int(not changed)

        _store_manifest_dirs(curr_dirs)

        res["wrote"] = wrote
        res["skipped"] = skipped
        return res

    except Exception as e:
        res["ok"] = False
        res["err"] = f"{e.__class__.__name__}: {e}"
        return res
    finally:
        os.chdir(cwd0)
        res["t_ms"] = (time.perf_counter() - t0) * 1000.0


if __name__ == "__main__":
    import cProfile, pstats
    import sys

    REPO_ROOT = Path(__file__).parent.parent.parent
    sys.path.append(str(REPO_ROOT))

    pr = cProfile.Profile()
    pr.enable()
    # call your entrypoint here:
    gen_auto_headers(Path("."))  # or whatever main flow
    pr.disable()
    ps = pstats.Stats(pr).sort_stats("cumtime")
    ps.print_stats(50)  # top 50 by cumulative time
    ps.sort_stats("tottime").print_stats(50)
