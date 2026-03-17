import concurrent.futures
import hashlib
import heapq
import json
import os
import platform
import re
import shlex
import subprocess
import sys
import tempfile
from typing import Any, Iterator

default_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
if not default_dir:
    print(
        "This script must be run though bazel. Please run 'bazel run //evergreen:validate_compile_commands' instead."
    )
    sys.exit(1)

os.chdir(default_dir)

if not os.path.exists("compile_commands.json"):
    sys.stderr.write("The 'compile_commands.json' file was not found.\n")
    sys.stderr.write("Attempting to run 'bazel build compiledb' to generate it.\n")
    subprocess.run(["bazel", "build", "compiledb"], check=True)


def _parse_repo_env_from_bazelrc(bazelrc_path: str, var_name: str) -> str | None:
    """Extract --repo_env=FOO=... from a .bazelrc file (best-effort)."""
    if not os.path.exists(bazelrc_path):
        return None
    # Example: common:windows --repo_env=BAZEL_VC="C:/Program Files/.../VC"
    pat = re.compile(rf"--repo_env={re.escape(var_name)}=(?:\"([^\"]+)\"|'([^']+)'|(\S+))")
    with open(bazelrc_path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = pat.search(line)
            if not m:
                continue
            val = m.group(1) or m.group(2) or m.group(3)
            if not val:
                continue
            # Bazelrc often uses forward slashes on Windows; normalize.
            return os.path.normpath(val)
    return None


def _capture_msvc_env(vs_vc_dir: str, arch: str) -> dict[str, str]:
    """Run vcvarsall.bat and capture the environment it sets."""
    # Some environments may include surrounding quotes in BAZEL_VC/BAZEL_VS.
    vs_vc_dir = vs_vc_dir.strip().strip('"').strip("'")
    candidates = [
        os.path.join(vs_vc_dir, "Auxiliary", "Build", "vcvarsall.bat"),
        # If caller gave VS install root instead of VC root.
        os.path.join(vs_vc_dir, "VC", "Auxiliary", "Build", "vcvarsall.bat"),
    ]
    vcvarsall = next((p for p in candidates if os.path.exists(p)), None)
    if not vcvarsall:
        raise FileNotFoundError(f"vcvarsall.bat not found under: {vs_vc_dir}")

    vcvarsall = os.path.normpath(vcvarsall).strip().strip('"').strip("'")

    def _run_cmd_capture_env(cmd: list[str]) -> dict[str, str]:
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(
                f"Failed to run vcvarsall.bat (rc={proc.returncode}). stderr:\n{proc.stderr}"
            )
        env: dict[str, str] = {}
        for line in proc.stdout.splitlines():
            if "=" not in line:
                continue
            k, v = line.split("=", 1)
            if k:
                env[k] = v
        return env

    # Use cmd.exe to run the batch file and then dump environment.
    # Avoid /s here because it changes quoting semantics in edge cases.
    try:
        return _run_cmd_capture_env(
            ["cmd.exe", "/d", "/c", f'call "{vcvarsall}" {arch} >nul && set']
        )
    except RuntimeError:
        # Fallback: write a small .cmd file to avoid tricky quoting issues.
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".cmd", delete=False, encoding="utf-8"
        ) as tf:
            tf.write("@echo off\n")
            tf.write(f'call "{vcvarsall}" {arch} >nul\n')
            tf.write("set\n")
            script_path = tf.name
        try:
            return _run_cmd_capture_env(["cmd.exe", "/d", "/c", script_path])
        finally:
            try:
                os.remove(script_path)
            except OSError:
                pass


def _maybe_add_windows_toolchain_env(base_env: dict[str, str], repo_root: str) -> dict[str, str]:
    """On Windows, ensure INCLUDE/LIB/PATH are set by loading a VS developer env."""
    if platform.system() != "Windows":
        return base_env

    bazelrc = os.path.join(repo_root, ".bazelrc")
    vc_root = base_env.get("BAZEL_VC") or _parse_repo_env_from_bazelrc(bazelrc, "BAZEL_VC")
    vs_root = base_env.get("BAZEL_VS") or _parse_repo_env_from_bazelrc(bazelrc, "BAZEL_VS")

    # Prefer explicit VC root, but fall back to VS root if that's what we have.
    vs_vc_dir = vc_root or vs_root
    if not vs_vc_dir:
        return base_env

    arch = "amd64"
    proc_arch = (base_env.get("PROCESSOR_ARCHITECTURE") or "").upper()
    if proc_arch and proc_arch != "AMD64":
        arch = "x86"

    print(f"Loading Visual Studio environment for test compiles (arch={arch})...", flush=True)
    try:
        msvc_env = _capture_msvc_env(vs_vc_dir, arch=arch)
    except Exception as e:
        sys.stderr.write(
            f"WARNING: Failed to load MSVC env from BAZEL_VC/BAZEL_VS ({vs_vc_dir}): {e}\n"
        )
        return base_env

    merged = dict(base_env)
    merged.update(msvc_env)
    return merged


def _iter_compiledb_entries(path: str) -> Iterator[dict[str, Any]]:
    """Stream parse compile_commands.json (a JSON array of objects) without loading it all in memory."""
    decoder = json.JSONDecoder()
    buf = ""
    pos = 0
    with open(path, "r", encoding="utf-8") as f:
        # Prime the buffer until we find the start of the array.
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                raise ValueError(f"{path} is empty or not valid JSON.")
            buf += chunk
            buf = buf.lstrip()
            if buf:
                break

        if not buf.startswith("["):
            raise ValueError(f"{path} does not start with a JSON array.")
        pos = 1  # skip '['

        while True:
            # Skip whitespace and commas.
            while pos < len(buf) and buf[pos] in " \r\n\t,":
                pos += 1

            # Refill buffer if needed.
            if pos >= len(buf):
                chunk = f.read(1024 * 1024)
                if not chunk:
                    raise ValueError(f"Unexpected EOF while parsing {path}.")
                buf += chunk
                continue

            if buf[pos] == "]":
                return

            try:
                obj, next_pos = decoder.raw_decode(buf, pos)
            except json.JSONDecodeError:
                # Likely split across chunk boundary; read more and retry.
                chunk = f.read(1024 * 1024)
                if not chunk:
                    raise ValueError(f"Unexpected EOF while parsing {path}.")
                # Drop consumed prefix to avoid unbounded growth.
                if pos > 0:
                    buf = buf[pos:]
                    pos = 0
                buf += chunk
                continue

            if not isinstance(obj, dict):
                raise ValueError(
                    f"Expected object entries in {path}, got {type(obj)} at pos {pos}."
                )
            yield obj
            pos = next_pos
            # Drop consumed prefix periodically to keep memory bounded.
            if pos > 1024 * 1024:
                buf = buf[pos:]
                pos = 0


def _hash_file_name(file_name: str) -> int:
    # Deterministic across runs; 'file' in compile_commands is typically relative and stable.
    digest = hashlib.sha256(file_name.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], byteorder="big", signed=False)


def _make_test_compile_args(args: list[str]) -> list[str]:
    """Convert a compile command into a 'test compile' command.

    By default we keep the compilation semantics (not syntax-only), but we can optionally
    force syntax-only with VALIDATE_COMPILE_COMMANDS_SYNTAX_ONLY=1.
    """
    if not args:
        return args

    compiler = os.path.basename(args[0]).lower()
    syntax_only = os.environ.get("VALIDATE_COMPILE_COMMANDS_SYNTAX_ONLY", "").strip() not in (
        "",
        "0",
        "false",
        "False",
    )

    if any(x in compiler for x in ["clang", "gcc", "g++", "c++"]):
        out = list(args)
        if syntax_only and "-fsyntax-only" not in out:
            out.append("-fsyntax-only")
        return out

    if compiler in ["cl", "cl.exe"]:
        out = list(args)
        if syntax_only and "/Zs" not in out:
            out.append("/Zs")
        return out

    return list(args)


def _map_writable_output_path(out_root: str, original_path: str) -> str:
    """Map an output path from compile_commands.json into a writable tree under out_root.

    Must be robust on Windows where absolute paths include a drive prefix like `C:\\...`
    (we cannot embed `:` as a path component under out_root).
    """

    def _sanitize_component(comp: str) -> str:
        # Windows-invalid characters: <>:"/\|?* (also avoid path separators).
        trans = str.maketrans({c: "_" for c in '<>:"/\\|?*'})
        comp = comp.translate(trans)
        # Windows: components cannot end with '.' or ' '.
        comp = comp.rstrip(". ")
        if comp in ("", ".", ".."):
            return "_"
        return comp

    drive, tail = os.path.splitdrive(original_path)
    parts: list[str] = []

    if drive:
        # Drive may be "C:" or a UNC prefix like "\\\\server\\share".
        drive_tag = drive.rstrip(":")
        drive_tag = drive_tag.lstrip("\\/").replace("\\", "_").replace("/", "_")
        drive_tag = _sanitize_component(drive_tag) if drive_tag else "DRIVE"
        parts.append(drive_tag)
        tail = tail.lstrip("\\/")
    else:
        tail = original_path.lstrip("\\/") if os.path.isabs(original_path) else original_path

    tail = tail.replace("\\", "/")
    for p in tail.split("/"):
        if p:
            parts.append(_sanitize_component(p))

    if not parts:
        parts = ["out"]

    return os.path.normpath(os.path.join(out_root, *parts))


def _rewrite_output_paths_to_writable_dir(
    args: list[str], cwd: str, out_root: str, entry_output: str | None = None
) -> list[str]:
    """Rewrite output-producing args (-o, -MF, /Fo, etc.) into a writable directory."""
    if not args:
        return args

    rewritten = list(args)

    def _norm_abs(p: str) -> str:
        # Normalize both Windows and POSIX-ish paths from compile_commands.json.
        abs_p = p if os.path.isabs(p) else os.path.join(cwd, p)
        return os.path.normcase(os.path.normpath(abs_p))

    # Collect all plausible output paths (compile_commands "output" can differ from actual /Fo).
    orig_outs: list[str] = []
    if isinstance(entry_output, str) and entry_output:
        orig_outs.append(entry_output)

    i = 0
    while i < len(rewritten):
        a = rewritten[i]
        if a == "-o" and i + 1 < len(rewritten):
            orig_outs.append(rewritten[i + 1])
            i += 2
            continue
        if a.startswith("-o") and len(a) > 2:
            orig_outs.append(a[2:])
        if a == "/Fo" and i + 1 < len(rewritten):
            orig_outs.append(rewritten[i + 1])
            i += 2
            continue
        if a.startswith("/Fo") and len(a) > 3:
            orig_outs.append(a[3:])
        i += 1

    # Build mapping by normalized absolute path to destination.
    out_map: dict[str, str] = {}
    dep_map: dict[str, str] = {}
    for o in orig_outs:
        if not o:
            continue
        o_abs = _norm_abs(o)
        if o_abs in out_map:
            continue
        dest_out = _map_writable_output_path(out_root, o_abs)
        # Ensure the output directory exists.
        os.makedirs(os.path.dirname(dest_out), exist_ok=True)
        out_map[o_abs] = dest_out

        dep_abs = os.path.splitext(o_abs)[0] + ".d"
        dest_dep = os.path.splitext(dest_out)[0] + ".d"
        os.makedirs(os.path.dirname(dest_dep), exist_ok=True)
        dep_map[dep_abs] = dest_dep

    i = 0
    while i < len(rewritten):
        a = rewritten[i]

        # Rewrite -o <path>
        if a == "-o" and i + 1 < len(rewritten):
            cand = _norm_abs(rewritten[i + 1])
            if cand in out_map:
                rewritten[i + 1] = out_map[cand]
            i += 2
            continue

        # Rewrite combined -o<path>
        if a.startswith("-o") and len(a) > 2:
            cand = _norm_abs(a[2:])
            if cand in out_map:
                rewritten[i] = "-o" + out_map[cand]
            i += 1
            continue

        # Rewrite depfile -MF <path> (if it matches the common "<outbase>.d" pattern).
        if a == "-MF" and i + 1 < len(rewritten):
            cand = _norm_abs(rewritten[i + 1])
            if cand in dep_map:
                rewritten[i + 1] = dep_map[cand]
            i += 2
            continue

        # Rewrite combined -MF<path>
        if a.startswith("-MF") and len(a) > 3:
            cand = _norm_abs(a[3:])
            if cand in dep_map:
                rewritten[i] = "-MF" + dep_map[cand]
            i += 1
            continue

        # Rewrite MSVC /Fo forms.
        if a == "/Fo" and i + 1 < len(rewritten):
            cand = _norm_abs(rewritten[i + 1])
            if cand in out_map:
                rewritten[i + 1] = out_map[cand]
            i += 2
            continue
        if a.startswith("/Fo") and len(a) > 3:
            cand = _norm_abs(a[3:])
            if cand in out_map:
                rewritten[i] = "/Fo" + out_map[cand]
            i += 1
            continue

        # Generic token replacement for exact matches (helps with toolchains that also
        # reference the output path elsewhere on the command line).
        if (
            ("/" in a)
            or ("\\" in a)
            or (":" in a)
            or a.startswith("bazel-out")
            or a.startswith("external")
        ):
            cand = _norm_abs(a)
            if cand in out_map:
                rewritten[i] = out_map[cand]
            elif cand in dep_map:
                rewritten[i] = dep_map[cand]

        i += 1

    return rewritten


def _ensure_parent_dirs_exist_for_outputs(args: list[str], cwd: str, repo_root: str) -> None:
    """Create parent dirs for any output/deps paths referenced by the command.

    This is intentionally conservative: it only creates directories for paths that resolve
    inside repo_root.
    """

    def _maybe_mkdir(path_str: str) -> None:
        if not path_str:
            return
        abs_path = path_str if os.path.isabs(path_str) else os.path.join(cwd, path_str)
        abs_path = os.path.normpath(abs_path)
        # Use real paths so we do not attempt to create directories through symlinks that
        # escape the repo (e.g. bazel output_base via .compiledb symlinks).
        abs_real = os.path.realpath(abs_path)
        repo_real = os.path.realpath(repo_root)
        try:
            if os.path.commonpath([repo_real, abs_real]) != repo_real:
                return
        except ValueError:
            # Different drives on Windows, etc.
            return
        parent = os.path.dirname(abs_path)
        if parent:
            try:
                os.makedirs(parent, exist_ok=True)
            except PermissionError:
                # In constrained environments (or when pointing into Bazel output_base),
                # directory creation may be denied. The test compile path-stripping is
                # intended to avoid requiring these outputs anyway.
                return

    i = 0
    while i < len(args):
        a = args[i]

        # GCC/Clang style paired flags.
        if a in ("-o", "-MF", "-MJ") and i + 1 < len(args):
            _maybe_mkdir(args[i + 1])
            i += 2
            continue

        # GCC/Clang combined forms.
        if a.startswith("-o") and len(a) > 2:
            _maybe_mkdir(a[2:])
        if a.startswith("-MF") and len(a) > 3:
            _maybe_mkdir(a[3:])
        if a.startswith("-MJ") and len(a) > 3:
            _maybe_mkdir(a[3:])

        # MSVC combined forms.
        if a.startswith("/Fo") and len(a) > 3:
            _maybe_mkdir(a[3:])
        if a.startswith("/Fd") and len(a) > 3:
            _maybe_mkdir(a[3:])

        i += 1


def _select_entries_for_test_compile(path: str, n: int) -> tuple[int, list[dict[str, Any]]]:
    """Pick N entries by sorting deterministic hashes of entry['file'] and taking the first N."""
    # Keep a max-heap of the N smallest hashes.
    # IMPORTANT: include stable, comparable tie-breakers so heapq never compares dicts.
    # Tuple: (-hash, file_name, seq, entry)
    heap: list[tuple[int, str, int, dict[str, Any]]] = []
    total = 0
    seq = 0
    for entry in _iter_compiledb_entries(path):
        total += 1
        file_name = entry.get("file")
        if not isinstance(file_name, str):
            continue
        h = _hash_file_name(file_name)
        item = (-h, file_name, seq, entry)
        seq += 1
        if len(heap) < n:
            heapq.heappush(heap, item)
        else:
            # If this hash is smaller than the current largest in the heap, replace it.
            if item[:3] > heap[0][:3]:
                heapq.heapreplace(heap, item)

    # Sort ascending by hash.
    selected = [
        e for (_neg_h, _file_name, _seq, e) in sorted(heap, key=lambda t: (-t[0], t[1], t[2]))
    ]
    return total, selected


def main() -> int:
    compdb_path = "compile_commands.json"
    total, selected = _select_entries_for_test_compile(compdb_path, n=10)

    if total < 1000:
        sys.stderr.write(
            f"ERROR: 'compile_commands.json' has less than 1000 entries. Found {total} entries.\n"
        )
        return 1

    if not selected:
        sys.stderr.write("ERROR: Failed to select any entries for test compilation.\n")
        return 1

    out_root = os.environ.get(
        "VALIDATE_COMPILE_COMMANDS_OUT_DIR",
        os.path.join(default_dir, ".validate_compile_commands_out"),
    )
    os.makedirs(out_root, exist_ok=True)

    def _prep_entry(entry: dict[str, Any]) -> tuple[str, str, list[str]]:
        args = entry.get("arguments")
        if args is None and isinstance(entry.get("command"), str):
            # Fallback for standard compile_commands format.
            args = shlex.split(entry["command"])
        directory = entry.get("directory")
        file_name = entry.get("file")
        output_path = entry.get("output")
        if not isinstance(file_name, str):
            file_name = "<unknown>"
        if not isinstance(args, list) or not all(isinstance(x, str) for x in args):
            return ("", "", [])
        if not isinstance(directory, str):
            return ("", "", [])
        test_args = _make_test_compile_args(args)
        if isinstance(output_path, str) and output_path:
            test_args = _rewrite_output_paths_to_writable_dir(
                test_args, cwd=directory, out_root=out_root, entry_output=output_path
            )
        else:
            test_args = _rewrite_output_paths_to_writable_dir(
                test_args, cwd=directory, out_root=out_root
            )
        return (file_name, directory, test_args)

    work: list[tuple[str, str, list[str]]] = []
    for entry in selected:
        file_name, directory, test_args = _prep_entry(entry)
        if file_name and directory and test_args:
            work.append((file_name, directory, test_args))

    if not work:
        sys.stderr.write("ERROR: No valid entries found for test compilation.\n")
        return 1

    jobs_env = os.environ.get("VALIDATE_COMPILE_COMMANDS_JOBS")
    max_workers = int(jobs_env) if jobs_env else (os.cpu_count() or 4)
    max_workers = max(1, min(max_workers, len(work)))

    print(f"Running {len(work)} test compiles...", flush=True)
    compile_env = _maybe_add_windows_toolchain_env(os.environ.copy(), repo_root=default_dir)

    def _run_one(item: tuple[str, str, list[str]]) -> tuple[str, int, list[str], str, str]:
        file_name, directory, test_args = item
        _ensure_parent_dirs_exist_for_outputs(test_args, cwd=directory, repo_root=default_dir)
        proc = subprocess.run(
            test_args, cwd=directory, env=compile_env, capture_output=True, text=True
        )
        return (file_name, proc.returncode, test_args, proc.stdout, proc.stderr)

    failures = 0
    remaining = len(work)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as ex:
        futures = [ex.submit(_run_one, item) for item in work]
        for fut in concurrent.futures.as_completed(futures):
            file_name, rc, test_args, out, err = fut.result()
            remaining -= 1
            print(f"{remaining} test compiles left...", flush=True)
            if rc != 0:
                failures += 1
                sys.stderr.write(f"ERROR: test compilation failed (rc={rc}) for file={file_name}\n")
                sys.stderr.write("Command:\n")
                sys.stderr.write(" ".join(test_args) + "\n")
                if out:
                    sys.stderr.write("--- stdout ---\n")
                    sys.stderr.write(out[-8000:] + "\n")
                if err:
                    sys.stderr.write("--- stderr ---\n")
                    sys.stderr.write(err[-8000:] + "\n")

    if failures:
        sys.stderr.write(f"ERROR: {failures} / {len(work)} test compilations failed.\n")
        return 1

    print(f"Successfully validated compile_commands.json file ({len(work)} test compilations).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
