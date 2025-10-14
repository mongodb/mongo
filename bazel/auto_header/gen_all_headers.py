#!/usr/bin/env python3
import os
import sys
import time
import threading
import subprocess
import hashlib, os, tempfile
from pathlib import Path

from pathlib import Path

# --- FAST-PATH HEADER LABEL GENERATOR (threaded) -----------------------------
explicit_includes = []


def _write_if_changed(out_path: Path, content: str, *, encoding="utf-8") -> bool:
    """
    Atomically write `content` to `out_path` iff bytes differ.
    Returns True if file was written, False if unchanged.
    """
    data = content.encode(encoding)

    # Fast path: compare size first
    try:
        st = out_path.stat()
        if st.st_size == len(data):
            # Same size; compare content via hash (chunked)
            h_existing = hashlib.sha256()
            with out_path.open("rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h_existing.update(chunk)
            h_new = hashlib.sha256(data).hexdigest()
            if h_existing.hexdigest() == h_new:
                return False  # identical; skip write
    except FileNotFoundError:
        pass

    # Different (or missing): write to temp then replace atomically
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(out_path.parent), prefix=out_path.name + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb", buffering=0) as f:
            f.write(data)
            # Optionally ensure durability:
            # f.flush(); os.fsync(f.fileno())
        os.replace(tmp, out_path)  # atomic on POSIX/Windows when same filesystem
    except Exception:
        # Best effort cleanup if something goes wrong
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    return True


def _gen_labels_from_fd(repo_root: Path) -> list[str]:
    """Stream fd output and return a list of raw labels like //pkg:file.h."""
    sys.path.append(str(repo_root))
    try:
        from bazel.auto_header.ensure_fd import ensure_fd  # returns str|Path|None
    except Exception:
        ensure_fd = lambda **_: None  # noqa: E731

    fd_path = ensure_fd()
    if not fd_path:
        return []  # caller will fall back to Python walk

    fd_path = str(fd_path)  # normalize in case ensure_fd returns a Path

    cmd = [
        fd_path,
        "-t",
        "f",
        "-0",
        "-g",
        "**.{h,hpp,hh,inc,ipp,idl,inl,defs}",
        "src/mongo",
        "-E",
        "third_party",
        "-E",
        "**/third_party/**",
    ]

    p = subprocess.Popen(
        cmd,
        cwd=repo_root,
        stdout=subprocess.PIPE,
        env=dict(os.environ, LC_ALL="C"),  # stable bytewise sort on POSIX
    )
    rd = p.stdout.read
    buf = bytearray()
    labels: list[str] = []
    append = labels.append

    while True:
        chunk = rd(1 << 16)
        if not chunk:
            break
        buf.extend(chunk)
        start = 0
        while True:
            try:
                i = buf.index(0, start)
            except ValueError:
                if start:
                    del buf[:start]
                break
            s = buf[start:i].decode("utf-8", "strict")
            start = i + 1
            if not s.startswith("src/mongo/"):
                continue
            slash = s.rfind("/")
            pkg = s[:slash]
            base = s[slash + 1 :]
            if base.endswith(".idl"):
                append(f"//{pkg}:{base[:-4]}_gen.h")  # file label
            elif base.endswith(".tpl.h"):
                append(f"//{pkg}:{base[:-6]}.h")
            else:
                append(f"//{pkg}:{base}")

    # Tail (rare)
    if buf:
        s = buf.decode("utf-8", "strict")
        if s.startswith("src/mongo/"):
            slash = s.rfind("/")
            pkg = s[:slash]
            base = s[slash + 1 :]
            if base.endswith(".idl"):
                append(f"//{pkg}:{base[:-4]}_gen.h")
            elif base.endswith(".tpl.h"):
                append(f"//{pkg}:{base[:-6]}.h")
            else:
                append(f"//{pkg}:{base}")

    p.wait()
    # De-dup & canonical sort
    labels = sorted(set(labels))
    return labels


def _gen_labels_pywalk(repo_root: Path) -> list[str]:
    """
    Pure-Python fallback → list of raw labels like //pkg:file.h,
    mirroring fd's filters and rewrites.
    """
    start_dir = repo_root / "src" / "mongo"  # match fd search root
    if not start_dir.exists():
        return []

    # Exact-name excludes, plus "bazel-*" prefix
    EXCLUDE_DIRS = {
        "third_party",  # exclude at any depth
    }

    # Simple-pass extensions (anything else is ignored unless handled below)
    PASS_EXTS = (".h", ".hpp", ".hh", ".inc", ".ipp", ".inl", ".defs")

    labels: list[str] = []
    append = labels.append
    root = str(repo_root)

    for dirpath, dirnames, filenames in os.walk(str(start_dir), topdown=True, followlinks=False):
        # Prune dirs in-place for speed and correctness
        dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]

        rel_dir = os.path.relpath(dirpath, root).replace("\\", "/")  # e.g. "src/mongo/...""

        for fn in filenames:
            # Rewrite rules first (more specific)
            if fn.endswith(".tpl.h"):
                # "foo.tpl.h" -> "foo.h"
                append(f"//{rel_dir}:{fn[:-6]}.h")
                continue
            if fn.endswith(".idl"):
                # "foo.idl" -> "foo_gen.h"
                append(f"//{rel_dir}:{fn[:-4]}_gen.h")
                continue

            # Pass-through if in the accepted set
            if fn.endswith(PASS_EXTS):
                append(f"//{rel_dir}:{fn}")

    # De-dup + stable sort to mirror fd pipeline
    return sorted(set(labels))


def _build_file_content(lines: str) -> str:
    return (
        'package(default_visibility = ["//visibility:public"])\n\n'
        "filegroup(\n"
        '    name = "all_headers",\n'
        "    srcs = [\n"
        f"{lines}"
        "    ],\n"
        ")\n"
    )


MODULES_PREFIX = "src/mongo/db/modules/"


def _stable_write(out_path: Path, content: str) -> bool:
    data = content.encode("utf-8")
    try:
        st = out_path.stat()
        if st.st_size == len(data):
            h_old = hashlib.sha256()
            with out_path.open("rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    h_old.update(chunk)
            if h_old.hexdigest() == hashlib.sha256(data).hexdigest():
                return False
    except FileNotFoundError:
        pass
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(out_path.parent), prefix=out_path.name + ".", suffix=".tmp")
    with os.fdopen(fd, "wb", buffering=0) as f:
        f.write(data)
    os.replace(tmp, out_path)
    return True


def _build_filegroup(lines: list[str], *, visibility: str | None = None) -> str:
    # lines must be sorted, each like:        "//pkg:thing",\n
    body = "".join(lines)
    vis = visibility or "//visibility:public"
    return (
        f'package(default_visibility = ["{vis}"])\n\n'
        "filegroup(\n"
        '    name = "all_headers",\n'
        "    srcs = [\n"
        f"{body}"
        "    ],\n"
        ")\n"
    )


def _bucket_label(label: str) -> tuple[str, str] | None:
    """
    Returns (bucket, module_name) where:
      bucket == "GLOBAL" for non-module files
      bucket == "<module>" for files under src/mongo/db/modules/<module>/
    `label` is like //src/mongo/db/modules/atlas/foo:bar.h
    """
    # peel off the leading // and split package and target
    if not label.startswith("//"):
        return None
    pkg = label[2:].split(":", 1)[0]  # e.g. src/mongo/db/modules/atlas/foo
    if pkg.startswith(MODULES_PREFIX):
        parts = pkg[len(MODULES_PREFIX) :].split("/", 1)
        if parts and parts[0]:
            return (parts[0], parts[0])  # (bucket, module)
    return ("GLOBAL", "")


def write_sharded_all_headers(repo_root: Path, labels: list[str]) -> dict[str, bool]:
    by_bucket: dict[str, list[str]] = {}
    for lbl in labels:
        buck, _ = _bucket_label(lbl) or ("GLOBAL", "")
        by_bucket.setdefault(buck, []).append(f'        "{lbl}",\n')

    results: dict[str, bool] = {}

    # GLOBAL
    global_lines = sorted(by_bucket.get("GLOBAL", []))
    global_out = repo_root / "bazel" / "auto_header" / ".auto_header" / "BUILD.bazel"
    results[str(global_out)] = _stable_write(global_out, _build_filegroup(global_lines))

    # modules
    for buck, lines in by_bucket.items():
        if buck == "GLOBAL":
            continue
        lines.sort()
        mod_dir = repo_root / "src" / "mongo" / "db" / "modules" / buck / ".auto_header"
        vis = f"//src/mongo/db/modules/{buck}:__subpackages__"
        outp = mod_dir / "BUILD.bazel"
        results[str(outp)] = _stable_write(outp, _build_filegroup(lines, visibility=vis))

    return results


def spawn_all_headers_thread(repo_root: Path) -> tuple[threading.Thread, dict]:
    state = {"ok": False, "t_ms": 0.0, "wrote": False, "err": None}

    def _worker():
        t0 = time.perf_counter()
        try:
            labels = _gen_labels_from_fd(repo_root)
            if not labels:
                labels = _gen_labels_pywalk(repo_root)

            for label in explicit_includes:
                bisect.insort(labels, label)

            wrote_any = False
            results = write_sharded_all_headers(repo_root, labels)
            # results: {path: True/False}
            wrote_any = any(results.values())

            state.update(ok=True, wrote=wrote_any)
        except Exception as e:
            state.update(ok=False, err=e)
        finally:
            state["t_ms"] = (time.perf_counter() - t0) * 1000.0

    th = threading.Thread(target=_worker, name="all-headers-gen", daemon=True)
    th.start()
    return th, state


# ---------------------------------------------------------------------------
