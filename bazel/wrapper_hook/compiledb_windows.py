"""Command-line replay support for the Windows cross compiler.

Compilation database entries launch this module with the host interpreter recorded
by the build. It supplies the action environment and translates host paths before
handing the compile to the existing persistent-container runner.
"""

from __future__ import annotations

import ast
import dataclasses
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
from typing import Any

CONTAINER_SHARED_INSTALL_SUFFIX = "-mongo-shared-install"
# cmd.exe rejects command lines of 8192 characters and beyond, before the wrapper
# gets control. Database generation must put longer commands into response files.
WINDOWS_CMD_LENGTH_LIMIT = 8000
RESPONSE_FILE_DIRNAME = "compiledb_rsp"
# Compiler options whose value is a path glued directly onto the flag. Bazel
# actions only pass execroot-relative paths, but compile_commands.json entries
# carry absolute Windows paths (for example ``/IZ:/b/b/execroot/_main/...``)
# that have to be translated when the replay compiler runs inside the Linux
# cross-action container.
WINDOWS_PATH_FLAG_PREFIXES = (
    "/external:I",
    "/clang:-I",
    "/FI",
    "/Fa",
    "/Fd",
    "/Fi",
    "/Fo",
    "/Fp",
    "/I",
    "/Yc",
    "/Yu",
    "--sysroot=",
    "-fprofile-instr-use=",
    "-idirafter",
    "-imsvc",
    "-include",
    "-iquote",
    "-isystem",
    "-I",
    "-o",
)
_WINDOWS_DRIVE_PATH_RE = re.compile(r"^([A-Za-z]):/")
_EMBEDDED_WINDOWS_DRIVE_PATH_RE = re.compile(r"(?<=[=\"'])([A-Za-z]):[\\/][^\"']*")
# Windows cross hosts run their link actions through Bazel's persistent-container strategy
# and this runner; the wrapper hook leaves the runner's config in the Bazel output base.
# Replaying compile commands through the same runner executes the cross compiler in the
# same container the build used.
WINDOWS_CONTAINER_ACTIONS_CONFIG_FILENAME = "mongo_linux_container_actions.json"
WINDOWS_CONTAINER_ACTION_RUNNER = os.path.join(
    "bazel", "toolchains", "cc", "mongo_windows_cross", "windows_container_action_wrapper.py"
)
WINDOWS_CROSS_TOOLCHAIN_REPO_MARKER = "mongo_windows_cross_toolchain"
# CreateProcess accepts 32767 characters and the runner appends its own docker arguments.
WINDOWS_CONTAINER_COMMAND_LENGTH_LIMIT = 24000
# Host variables the runner process itself needs; everything else the compiler sees comes
# from the toolchain, exactly as in a Bazel action.
WINDOWS_CONTAINER_REPLAY_HOST_ENV = frozenset(
    {
        "APPDATA",
        "COMSPEC",
        "LOCALAPPDATA",
        "PATHEXT",
        "PROGRAMDATA",
        "SYSTEMDRIVE",
        "SYSTEMROOT",
        "TEMP",
        "TMP",
        "USERPROFILE",
    }
)
_WINDOWS_CROSS_INCLUDE_DIRS_RE = re.compile(
    r"cxx_builtin_include_directories\s*=\s*(\[[^\]]*\])", re.DOTALL
)
_WINDOWS_CROSS_LIB_ENV_RE = re.compile(r'msvc_env_lib\s*=\s*"([^"]*)"')


def _container_drive_path(match: "re.Match[str]", mount_prefix: str = "") -> str:
    return f"{mount_prefix}/{match.group(1).lower()}/"


def _container_replay_arg(arg: str, mount_prefix: str = "") -> str:
    """Rewrites absolute Windows paths in a cross-replay argument to container paths.

    The cross compiler runs inside a Linux container that exposes every host drive
    at ``<mount_prefix>/<drive letter>``: ``/mnt/<drive>`` for the persistent action
    container, ``/<drive>`` for the legacy batch-wrapper container. Only positions
    that are known to hold a path are rewritten so that values such as URLs in
    preprocessor definitions are left alone.
    """

    def drive_path(match: "re.Match[str]") -> str:
        return _container_drive_path(match, mount_prefix)

    if arg.startswith("@"):
        return "@" + _container_replay_arg(arg[1:], mount_prefix)
    normalized = arg.replace("\\", "/")
    if _WINDOWS_DRIVE_PATH_RE.match(normalized):
        return _WINDOWS_DRIVE_PATH_RE.sub(drive_path, normalized, count=1)
    for prefix in WINDOWS_PATH_FLAG_PREFIXES:
        if normalized.startswith(prefix):
            value = normalized[len(prefix) :]
            if _WINDOWS_DRIVE_PATH_RE.match(value):
                return prefix + _WINDOWS_DRIVE_PATH_RE.sub(drive_path, value, count=1)
            return normalized
    # Preserve escapes in macro values and other non-path arguments. Normalize only
    # the embedded path, rather than every backslash in the surrounding argument.
    return _EMBEDDED_WINDOWS_DRIVE_PATH_RE.sub(
        lambda match: drive_path(match) + match.group(0)[3:].replace("\\", "/"), arg
    )


def _windows_cross_toolchain_env_dirs(toolchain_dir: str) -> tuple[list[str], list[str]]:
    """Reads the INCLUDE and LIB directories the cross toolchain gives its compile actions.

    The toolchain repository rule writes them into the generated BUILD file as the
    ``cxx_builtin_include_directories`` list and the ``msvc_env_lib`` string; both are
    execroot-relative, like the environment of a real action.
    """
    build_file = os.path.join(toolchain_dir, "BUILD.bazel")
    try:
        with open(build_file, encoding="utf-8") as handle:
            text = handle.read()
    except OSError as exc:
        raise RuntimeError(
            f"cannot read the Windows cross toolchain BUILD file {build_file}: {exc}"
        ) from exc
    include_match = _WINDOWS_CROSS_INCLUDE_DIRS_RE.search(text)
    lib_match = _WINDOWS_CROSS_LIB_ENV_RE.search(text)
    if include_match is None or lib_match is None:
        raise RuntimeError(
            f"{build_file} does not declare cxx_builtin_include_directories and msvc_env_lib"
        )
    try:
        include_dirs = ast.literal_eval(include_match.group(1))
    except (SyntaxError, ValueError) as exc:
        raise RuntimeError(f"{build_file}: unreadable cxx_builtin_include_directories") from exc
    if not isinstance(include_dirs, list) or not all(isinstance(d, str) for d in include_dirs):
        raise RuntimeError(f"{build_file}: cxx_builtin_include_directories is not a string list")
    lib_dirs = [directory for directory in lib_match.group(1).split(";") if directory]
    return include_dirs, lib_dirs


@dataclasses.dataclass(frozen=True)
class WindowsContainerCompile:
    """Run the cross compiler with the build's container and toolchain environment."""

    workspace_dir: str
    output_base: str
    config_path: str
    config: dict[str, Any]
    toolchain_dir: str

    @property
    def runner(self) -> str:
        return os.path.join(self.workspace_dir, WINDOWS_CONTAINER_ACTION_RUNNER)

    @property
    def python(self) -> str:
        """Prefer the build's host interpreter, which works outside Bazel's runfiles."""
        return str(self.config.get("runner_python") or sys.executable)

    @property
    def mount_prefix(self) -> str:
        return str(self.config.get("drive_mount_prefix") or "/mnt").rstrip("/")

    @property
    def execroot(self) -> str:
        return os.path.join(self.output_base, "execroot", "_main")

    @property
    def shared_install_dir(self) -> str:
        configured = self.config.get("shared_install_dir")
        if isinstance(configured, str) and configured:
            return configured
        return self.output_base + CONTAINER_SHARED_INSTALL_SUFFIX

    def translate(self, arg: str) -> str:
        return _container_replay_arg(arg, self.mount_prefix)

    def container_path(self, path: str) -> str:
        """Resolve an action path without relying on the execroot's external junction."""
        normalized = path.replace("\\", "/")
        if normalized.startswith("external/"):
            host_path = os.path.join(self.output_base, normalized)
        elif os.path.isabs(path) or _WINDOWS_DRIVE_PATH_RE.match(normalized):
            host_path = path
        else:
            host_path = os.path.join(self.execroot, path)
        return self.translate(os.path.normpath(host_path))

    def replay_overlay_args(self, args: list[str]) -> list[str]:
        """Use an absolute-path copy of the toolchain's execroot-relative VFS overlay."""
        original_path = os.path.join(self.toolchain_dir, "case_insensitive_vfs.yaml")
        original_key = os.path.normcase(os.path.normpath(original_path))
        indices = [
            index
            for index, arg in enumerate(args)
            if os.path.normcase(os.path.normpath(arg)) == original_key
        ]
        if not indices:
            return args
        with open(original_path, encoding="utf-8") as handle:
            overlay = json.load(handle)

        def rewrite_node(node: dict[str, Any], *, root: bool = False) -> None:
            if root:
                node["name"] = self.container_path(node["name"])
            if "external-contents" in node:
                node["external-contents"] = self.container_path(node["external-contents"])
            for child in node.get("contents", []):
                rewrite_node(child)

        for node in overlay["roots"]:
            rewrite_node(node, root=True)
        contents = json.dumps(overlay, sort_keys=True) + "\n"
        digest = hashlib.sha256(contents.encode("utf-8")).hexdigest()
        overlay_dir = os.path.join(self.shared_install_dir, "compiledb_vfs")
        os.makedirs(overlay_dir, exist_ok=True)
        replay_path = os.path.join(overlay_dir, digest + ".json")
        if not os.path.isfile(replay_path):
            # Multiple replay compiles can create the same overlay concurrently.
            # Publish a complete file atomically, so no compiler reads a partial copy.
            descriptor, temporary_path = tempfile.mkstemp(dir=overlay_dir)
            try:
                with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write(contents)
                os.replace(temporary_path, replay_path)
            finally:
                if os.path.exists(temporary_path):
                    os.unlink(temporary_path)
        rewritten = list(args)
        for index in indices:
            rewritten[index] = replay_path
        return rewritten

    def _container_env_path_list(self, entries: list[str]) -> str:
        """Rewrites execroot-relative toolchain paths into absolute container paths.

        The container cannot resolve relative entries against the translated working
        directory (they are Windows-style paths against a path list separator that the
        compiler reads with Windows semantics), and toolchain repositories live in the
        output base rather than behind the execroot's junctions. clang-cl splits the
        INCLUDE and LIB variables on ``;`` only, so the rewritten list keeps that
        separator.
        """
        return ";".join(self.container_path(entry) for entry in entries)

    def environment(self, base_env: dict[str, str]) -> dict[str, str]:
        """Returns the replay environment: host essentials plus the toolchain's action env."""
        include_dirs, lib_dirs = _windows_cross_toolchain_env_dirs(self.toolchain_dir)
        environment = {
            key: value
            for key, value in base_env.items()
            if key.upper() in WINDOWS_CONTAINER_REPLAY_HOST_ENV
        }
        environment["INCLUDE"] = self._container_env_path_list(include_dirs)
        environment["LIB"] = self._container_env_path_list(lib_dirs)
        # The runner is a Windows Python process and needs the host PATH to start; the
        # compiler inside the container does not consult PATH.
        environment["PATH"] = base_env.get("PATH", "")
        return environment

    def command(self, test_args: list[str]) -> list[str]:
        """The host command that runs ``test_args`` inside the container."""
        return [
            self.python,
            self.runner,
            self.config_path,
            test_args[0],
            *(self.translate(arg) for arg in test_args[1:]),
        ]


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

    output_flags = ("-MF", "-MJ", "/Fo", "/Fd", "/Fa", "/Fi", "-o")
    i = 0
    while i < len(rewritten):
        a = rewritten[i]
        if a in output_flags and i + 1 < len(rewritten):
            orig_outs.append(rewritten[i + 1])
            i += 2
            continue
        for prefix in output_flags:
            if a.startswith(prefix) and len(a) > len(prefix):
                orig_outs.append(a[len(prefix) :])
                break
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

        if a in output_flags and i + 1 < len(rewritten):
            cand = _norm_abs(rewritten[i + 1])
            if cand in out_map:
                rewritten[i + 1] = out_map[cand]
            i += 2
            continue
        combined_output = False
        for prefix in output_flags:
            if a.startswith(prefix) and len(a) > len(prefix):
                cand = _norm_abs(a[len(prefix) :])
                if cand in out_map:
                    rewritten[i] = prefix + out_map[cand]
                combined_output = True
                break
        if combined_output:
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


def _response_file_contents(args: list[str]) -> str:
    """clang-cl uses Windows response-file quoting even when it runs on Linux."""
    return subprocess.list2cmdline(args) + "\n"


def _split_response_file(contents: str) -> list[str]:
    """Parse the Windows quoting emitted by list2cmdline, preserving macro escapes."""
    arguments: list[str] = []
    index = 0
    while index < len(contents):
        while index < len(contents) and contents[index].isspace():
            index += 1
        if index == len(contents):
            break
        argument: list[str] = []
        quoted = False
        while index < len(contents) and (quoted or not contents[index].isspace()):
            slashes = 0
            while index < len(contents) and contents[index] == "\\":
                slashes += 1
                index += 1
            if index < len(contents) and contents[index] == '"':
                argument.extend("\\" * (slashes // 2))
                if slashes % 2:
                    argument.append('"')
                else:
                    quoted = not quoted
                index += 1
            else:
                argument.extend("\\" * slashes)
                if index < len(contents) and (quoted or not contents[index].isspace()):
                    argument.append(contents[index])
                    index += 1
        arguments.append("".join(argument))
    return arguments


def _expand_response_files(args: list[str], directory: str) -> list[str]:
    expanded: list[str] = []
    for arg in args:
        if arg.startswith("@"):
            path = os.path.join(directory, arg[1:])
            with open(path, encoding="utf-8") as handle:
                # Database generation writes one flat response file, with no nested files.
                expanded.extend(_split_response_file(handle.read()))
        else:
            expanded.append(arg)
    return expanded


def is_wrapped_command(args: list[str]) -> bool:
    return len(args) >= 4 and os.path.basename(args[1]) == "compiledb_windows.py"


def validation_command(args: list[str], output_dir: str | None = None) -> list[str]:
    """Request temporary outputs through the recorded wrapper's public CLI."""
    options = ["--output-dir", output_dir] if output_dir is not None else ["--validate"]
    return [*args[:2], *options, *args[2:]]


def wrap_windows_compile_commands(
    entries: list[dict[str, Any]], workspace_dir: str
) -> list[dict[str, Any]]:
    """Record runnable container compiler commands, including shell-safe long commands."""
    if platform.system() != "Windows":
        return entries
    for entry in entries:
        args = entry["arguments"]
        compiler = args[0].replace("\\", "/")
        if WINDOWS_CROSS_TOOLCHAIN_REPO_MARKER not in compiler or "/external/" not in compiler:
            continue
        output_base, remainder = compiler.split("/external/", 1)
        toolchain_dir = os.path.join(output_base, "external", remainder.split("/", 1)[0])
        config_path = os.path.join(output_base, WINDOWS_CONTAINER_ACTIONS_CONFIG_FILENAME)
        with open(config_path, encoding="utf-8") as handle:
            config = json.load(handle)
        if config.get("host_system") != "Windows":
            raise RuntimeError(f"{config_path} is not a Windows container configuration")
        replay = WindowsContainerCompile(
            workspace_dir, output_base, config_path, config, toolchain_dir
        )
        if not os.path.isfile(replay.runner):
            raise RuntimeError(f"Windows container runner does not exist: {replay.runner}")
        # Check the environment while generating, so a malformed toolchain does
        # not leave a database of commands that can never run.
        _windows_cross_toolchain_env_dirs(toolchain_dir)
        out_root = os.path.join(replay.shared_install_dir, "compiledb_out")
        args = _rewrite_output_paths_to_writable_dir(
            args, entry["directory"], out_root, entry.get("output")
        )
        if entry.get("output"):
            original = os.path.normcase(
                os.path.normpath(os.path.join(entry["directory"], entry["output"]))
            )
            entry["output"] = _map_writable_output_path(out_root, original)
        wrapper = os.path.join(workspace_dir, "bazel", "wrapper_hook", "compiledb_windows.py")
        command = [replay.python, wrapper, config_path, *args]
        # A shell can expand macro values before the wrapper starts. Keep those
        # values in a response file as well as commands too long for cmd.exe.
        shell_sensitive = any(any(char in arg for char in '%!&|<>^"') for arg in args[1:])
        if shell_sensitive or len(subprocess.list2cmdline(command)) > WINDOWS_CMD_LENGTH_LIMIT:
            response_dir = os.path.join(replay.shared_install_dir, RESPONSE_FILE_DIRNAME)
            os.makedirs(response_dir, exist_ok=True)
            contents = _response_file_contents(args[1:])
            digest = hashlib.sha256(contents.encode("utf-8")).hexdigest()
            response_path = os.path.join(response_dir, digest + ".rsp")
            with open(response_path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(contents)
            command = [replay.python, wrapper, config_path, args[0], "@" + response_path]
        entry["arguments"] = command
    return entries


def _ensure_output_directories(args: list[str]) -> None:
    for index, arg in enumerate(args):
        path = None
        if arg in ("-o", "-MF", "-MJ", "/Fo", "/Fd", "/Fa", "/Fi") and index + 1 < len(args):
            path = args[index + 1]
        else:
            for prefix in ("-MF", "-MJ", "/Fo", "/Fd", "/Fa", "/Fi", "-o"):
                if arg.startswith(prefix) and len(arg) > len(prefix):
                    path = arg[len(prefix) :]
                    break
        if path:
            parent = os.path.dirname(os.path.abspath(path))
            os.makedirs(parent, exist_ok=True)


def main(argv: list[str] | None = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir", help="Write temporary compilation outputs under this directory"
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="Write validation outputs under the container's writable shared directory",
    )
    parser.add_argument("config")
    parser.add_argument("compiler")
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    options = parser.parse_args(argv)
    try:
        with open(options.config, encoding="utf-8") as handle:
            config = json.load(handle)
        if config.get("host_system") != "Windows":
            raise RuntimeError("expected a Windows container configuration")
        output_base = os.path.dirname(os.path.abspath(options.config))
        compiler = options.compiler.replace("\\", "/")
        repo_name = compiler.split("/external/", 1)[1].split("/", 1)[0]
        workspace_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        replay = WindowsContainerCompile(
            workspace_dir,
            output_base,
            options.config,
            config,
            os.path.join(output_base, "external", repo_name),
        )
        args = [options.compiler, *_expand_response_files(options.arguments, os.getcwd())]
        output_dir = options.output_dir
        if output_dir is None and options.validate:
            output_dir = os.path.join(replay.shared_install_dir, "validate_compile_commands_out")
        if output_dir is not None:
            args = _rewrite_output_paths_to_writable_dir(args, os.getcwd(), output_dir)
        args = replay.replay_overlay_args(args)
        _ensure_output_directories(args[1:])
        environment = replay.environment(dict(os.environ))
        command = replay.command(args)
        # The wrapper itself was launched successfully; this limit protects its child
        # Python/Docker invocation, which uses CreateProcess rather than cmd.exe.
        if len(subprocess.list2cmdline(command)) <= WINDOWS_CONTAINER_COMMAND_LENGTH_LIMIT:
            return subprocess.run(command, env=environment, check=False).returncode
        response_dir = os.path.join(replay.shared_install_dir, RESPONSE_FILE_DIRNAME)
        os.makedirs(response_dir, exist_ok=True)
        descriptor, response_path = tempfile.mkstemp(dir=response_dir, suffix=".rsp")
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(_response_file_contents(command[4:]))
            return subprocess.run(
                [*command[:4], "@" + replay.translate(response_path)],
                env=environment,
                check=False,
            ).returncode
        finally:
            os.unlink(response_path)
    except (OSError, ValueError, RuntimeError, IndexError, KeyError, TypeError) as exc:
        print(f"ERROR: cannot replay Windows container compile: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
