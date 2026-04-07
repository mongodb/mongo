import argparse
import concurrent.futures
import hashlib
import heapq
import json
import os
import platform
import random
import re
import shlex
import subprocess
import sys
import tempfile
from typing import Any, Iterator

STANDARD_COMPILE_COMMAND_KEYS = frozenset({"arguments", "command", "directory", "file", "output"})
COMPILEDB_GENERATION_TARGETS = ["compiledb", "install-wiredtiger"]
MONGO_TIDY_PLUGIN_CANDIDATES = frozenset(
    [
        "libmongo_tidy_checks.so",
        "libmongo_tidy_checks.dylib",
        "mongo_tidy_checks.dll",
        "libmongo_tidy_checks.dll",
    ]
)


def _get_bazel_binary() -> str:
    return os.environ.get("BAZEL_BINARY", "bazel")


def _get_workspace_dir() -> str:
    workspace_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if workspace_dir:
        return workspace_dir
    raise RuntimeError(
        "This script must be run through bazel. "
        "Please run 'bazel run //evergreen:validate_compile_commands' instead."
    )


def _ensure_compiledb_exists(compdb_path: str) -> None:
    if os.path.exists(compdb_path):
        return
    sys.stderr.write(f"The '{compdb_path}' file was not found.\n")
    sys.stderr.write(
        "Attempting to run "
        f"'bazel build {' '.join(COMPILEDB_GENERATION_TARGETS)}' to generate it.\n"
    )
    subprocess.run([_get_bazel_binary(), "build", *COMPILEDB_GENERATION_TARGETS], check=True)


def _mongo_tidy_checks_supported_platform() -> bool:
    if platform.system() != "Linux":
        return False


def _validate_clang_tidy_setup(workspace_dir: str) -> None:
    if not _mongo_tidy_checks_supported_platform():
        return

    config_path = os.path.join(workspace_dir, ".clang-tidy")
    if not os.path.isfile(config_path):
        raise ValueError(
            "Expected '.clang-tidy' to exist in the workspace root after generating "
            "compile_commands.json on this platform."
        )

    plugin_marker_path = os.path.join(workspace_dir, ".mongo_checks_module_path")
    if not os.path.isfile(plugin_marker_path):
        raise ValueError(
            "Expected '.mongo_checks_module_path' to exist in the workspace root after "
            "generating compile_commands.json on this platform."
        )

    with open(plugin_marker_path, "r", encoding="utf-8") as marker_file:
        plugin_path = marker_file.read().strip()

    if not plugin_path:
        raise ValueError("'.mongo_checks_module_path' must contain a plugin path.")

    plugin_name = os.path.basename(plugin_path)
    if plugin_name not in MONGO_TIDY_PLUGIN_CANDIDATES:
        raise ValueError(
            f"'.mongo_checks_module_path' points to an unexpected plugin file: {plugin_name}"
        )

    if not os.path.isfile(plugin_path):
        raise ValueError(
            f"The mongo_tidy_checks plugin file recorded in '.mongo_checks_module_path' "
            f"does not exist: {plugin_path}"
        )


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


def _validate_compiledb_entry(entry: dict[str, Any], *, index: int) -> None:
    extra_keys = sorted(set(entry) - STANDARD_COMPILE_COMMAND_KEYS)
    if extra_keys:
        raise ValueError(
            f"compile_commands.json entry {index} has non-standard keys {extra_keys}. "
            f"Only {sorted(STANDARD_COMPILE_COMMAND_KEYS)} are allowed by the Clang JSON "
            "Compilation Database format."
        )

    directory = entry.get("directory")
    if not isinstance(directory, str) or not directory:
        raise ValueError(
            f"compile_commands.json entry {index} must contain a non-empty string 'directory'."
        )

    file_name = entry.get("file")
    if not isinstance(file_name, str) or not file_name:
        raise ValueError(
            f"compile_commands.json entry {index} must contain a non-empty string 'file'."
        )

    has_arguments = "arguments" in entry
    has_command = "command" in entry
    if has_arguments == has_command:
        raise ValueError(
            f"compile_commands.json entry {index} must contain exactly one of "
            "'arguments' or 'command'."
        )

    if has_arguments:
        arguments = entry["arguments"]
        if (
            not isinstance(arguments, list)
            or not arguments
            or not all(isinstance(arg, str) for arg in arguments)
        ):
            raise ValueError(
                f"compile_commands.json entry {index} 'arguments' must be a non-empty "
                "list of strings."
            )

    if has_command:
        command = entry["command"]
        if not isinstance(command, str) or not command:
            raise ValueError(
                f"compile_commands.json entry {index} 'command' must be a non-empty string."
            )

    if "output" in entry:
        output = entry["output"]
        if not isinstance(output, str) or not output:
            raise ValueError(
                f"compile_commands.json entry {index} 'output' must be a non-empty string "
                "when present."
            )


def _hash_file_name(file_name: str) -> int:
    # Deterministic across runs; 'file' in compile_commands is typically relative and stable.
    digest = hashlib.sha256(file_name.encode("utf-8")).digest()
    return int.from_bytes(digest[:8], byteorder="big", signed=False)


def _selection_key_for_entry(entry: dict[str, Any]) -> str | None:
    """Build a canonical key for cross-platform deterministic selection.

    compile_commands entries may use absolute paths rooted in machine-specific Bazel
    output locations (e.g. /tmp/.../external/... or Z:/.../bazel-out/<config>/...).
    We normalize those prefixes so the same logical source tends to hash the same
    across Linux/Windows.
    """
    file_name = entry.get("file")
    if not isinstance(file_name, str):
        return None

    directory = entry.get("directory")
    p = file_name.replace("\\", "/")
    # Windows paths are case-insensitive; lowercasing also improves cross-OS stability.
    p = p.lower()

    # If this is an absolute path under the entry directory, strip that prefix.
    if isinstance(directory, str):
        d = directory.replace("\\", "/").lower().rstrip("/")
        if d and p.startswith(d + "/"):
            p = p[len(d) + 1 :]

    # Strip machine-specific prefixes while preserving meaningful roots.
    if "/execroot/_main/" in p:
        p = p.split("/execroot/_main/", 1)[1]
    elif "/external/" in p:
        p = "external/" + p.split("/external/", 1)[1]
    elif "/bazel-out/" in p:
        p = "bazel-out/" + p.split("/bazel-out/", 1)[1]
    elif "/src/" in p:
        p = "src/" + p.split("/src/", 1)[1]

    # Normalize bazel configuration segment (platform/config differs by OS).
    p = re.sub(r"(^|/)bazel-out/[^/]+/", r"\1bazel-out/<config>/", p)
    p = re.sub(r"/+", "/", p).lstrip("./")
    return p or file_name.lower()


def _is_truthy_env(value: str | None) -> bool:
    if value is None:
        return False
    return value.strip().lower() not in ("", "0", "false", "no", "off")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    selection_group = parser.add_mutually_exclusive_group()
    selection_group.add_argument(
        "--run-all",
        action="store_true",
        help="Validate every compile_commands entry instead of sampling.",
    )
    selection_group.add_argument(
        "--sample-size",
        type=int,
        help="Validate a fixed number of compile_commands entries.",
    )
    return parser.parse_args()


def _determine_selection_count(
    default_count: int = 10,
    *,
    cli_run_all: bool = False,
    cli_sample_size: int | None = None,
) -> int:
    """Resolve how many compile_commands entries to test.

    CLI flags take precedence over environment variables.

    - --run-all: run all entries.
    - --sample-size N: run N entries (N > 0).
    - VALIDATE_COMPILE_COMMANDS_RUN_ALL=1: run all entries.
    - VALIDATE_COMPILE_COMMANDS_SAMPLE_SIZE=<N>: run N entries (N > 0).
    """
    if cli_run_all:
        return 0

    if cli_sample_size is not None:
        if cli_sample_size <= 0:
            raise ValueError(f"--sample-size must be > 0, got: {cli_sample_size}")
        return cli_sample_size

    if _is_truthy_env(os.environ.get("VALIDATE_COMPILE_COMMANDS_RUN_ALL")):
        return 0

    sample_size_env = os.environ.get("VALIDATE_COMPILE_COMMANDS_SAMPLE_SIZE")
    if sample_size_env is None:
        return default_count
    try:
        sample_size = int(sample_size_env)
    except ValueError as exc:
        raise ValueError(
            f"VALIDATE_COMPILE_COMMANDS_SAMPLE_SIZE must be an integer, got: {sample_size_env!r}"
        ) from exc
    if sample_size <= 0:
        raise ValueError(f"VALIDATE_COMPILE_COMMANDS_SAMPLE_SIZE must be > 0, got: {sample_size}")
    return sample_size


def _should_validate_entry(entry: dict[str, Any]) -> bool:
    selection_key = _selection_key_for_entry(entry)
    if not selection_key:
        return False

    # Keep the sample focused on MongoDB workspace sources. External repositories and
    # vendored third-party code have their own generated include layouts that are not
    # meaningful for validating the repo's compile_commands coverage.
    return selection_key.startswith("src/mongo/")


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

    if platform.system() == "Windows":
        drive, _ = os.path.splitdrive(original_path)
        drive_tag = drive.rstrip(":")
        drive_tag = drive_tag.lstrip("\\/").replace("\\", "_").replace("/", "_")
        drive_tag = _sanitize_component(drive_tag) if drive_tag else "PATH"

        normalized = os.path.normcase(os.path.normpath(original_path))
        digest = hashlib.sha256(normalized.encode("utf-8")).hexdigest()[:16]
        basename = _sanitize_component(os.path.basename(original_path)) or "out"
        stem, ext = os.path.splitext(basename)
        if len(stem) > 48:
            stem = stem[:48]
        short_name = f"{stem}-{digest}{ext}"

        return os.path.normpath(os.path.join(out_root, "win", drive_tag, short_name))

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
    """Pick N entries by sorting deterministic hashes of a canonicalized file key."""
    if n <= 0:
        total = 0
        selected: list[dict[str, Any]] = []
        for index, entry in enumerate(_iter_compiledb_entries(path), start=1):
            _validate_compiledb_entry(entry, index=index)
            total += 1
            file_name = entry.get("file")
            if not isinstance(file_name, str):
                continue
            selected.append(entry)
        return total, selected

    # Keep a max-heap of the N smallest hashes.
    # IMPORTANT: include stable, comparable tie-breakers so heapq never compares dicts.
    # Tuple: (-hash, selection_key, file_name, seq, entry)
    heap: list[tuple[int, str, str, int, dict[str, Any]]] = []
    total = 0
    seq = 0
    for index, entry in enumerate(_iter_compiledb_entries(path), start=1):
        _validate_compiledb_entry(entry, index=index)
        total += 1
        file_name = entry.get("file")
        if not isinstance(file_name, str):
            continue
        selection_key = _selection_key_for_entry(entry)
        if not selection_key or not _should_validate_entry(entry):
            continue
        h = _hash_file_name(selection_key)
        item = (-h, selection_key, file_name, seq, entry)
        seq += 1
        if len(heap) < n:
            heapq.heappush(heap, item)
        else:
            # If this hash is smaller than the current largest in the heap, replace it.
            if item[:4] > heap[0][:4]:
                heapq.heapreplace(heap, item)

    # Sort ascending by hash.
    selected = [
        e
        for (_neg_h, _selection_key, _file_name, _seq, e) in sorted(
            heap, key=lambda t: (-t[0], t[1], t[2], t[3])
        )
    ]
    return total, selected


def main() -> int:
    try:
        workspace_dir = _get_workspace_dir()
    except RuntimeError as e:
        print(e)
        return 1

    os.chdir(workspace_dir)

    cli_args = _parse_args()
    compdb_path = "compile_commands.json"
    _ensure_compiledb_exists(compdb_path)
    try:
        _validate_clang_tidy_setup(workspace_dir)
    except ValueError as e:
        sys.stderr.write(f"ERROR: {e}\n")
        return 1
    try:
        selection_count = _determine_selection_count(
            default_count=10,
            cli_run_all=cli_args.run_all,
            cli_sample_size=cli_args.sample_size,
        )
    except ValueError as e:
        sys.stderr.write(f"ERROR: {e}\n")
        return 1

    try:
        total, selected = _select_entries_for_test_compile(compdb_path, n=selection_count)
    except ValueError as e:
        sys.stderr.write(f"ERROR: {e}\n")
        return 1
    if selection_count <= 0:
        random.shuffle(selected)

    if total < 1000:
        sys.stderr.write(
            f"ERROR: 'compile_commands.json' has less than 1000 entries. Found {total} entries.\n"
        )
        return 1

    if not selected:
        sys.stderr.write("ERROR: Failed to select any entries for test compilation.\n")
        return 1

    if selection_count <= 0:
        print(
            f"Selected all compile_commands entries for validation ({len(selected)}).", flush=True
        )
    else:
        print(f"Selected {len(selected)} compile_commands entries for validation.", flush=True)

    out_root = os.environ.get(
        "VALIDATE_COMPILE_COMMANDS_OUT_DIR",
        os.path.join(workspace_dir, ".validate_compile_commands_out"),
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
    compile_env = _maybe_add_windows_toolchain_env(os.environ.copy(), repo_root=workspace_dir)

    def _run_one(item: tuple[str, str, list[str]]) -> tuple[str, int, list[str], str, str]:
        file_name, directory, test_args = item
        _ensure_parent_dirs_exist_for_outputs(test_args, cwd=directory, repo_root=workspace_dir)
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
