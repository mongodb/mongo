import errno
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

from bazel.wrapper_hook.compiledb_postprocess import (
    compile_command_sort_key,
    load_compile_command_fragments,
    load_compile_command_fragments_from_paths,
    write_compile_commands,
)
from bazel.wrapper_hook.write_wrapper_hook_bazelrc import write_wrapper_hook_bazelrc
from buildscripts.setup_clang_tidy import PLUGIN_CANDIDATES, materialize_clang_tidy_ide_files

COMPILEDB_START_TIME = time.monotonic()
COMPILEDB_POSTHOOK_STATE = REPO_ROOT / ".compiledb" / "posthook_state.json"
COMPILEDB_BUILD_TAG_FILTERS = "--build_tag_filters=mongo_compiledb"
COMPILEDB_REQUIRED_OUTPUT_REGEX = (
    r".*(_virtual_includes|_virtual_imports)/.*"
    r"|.*\.(compile_command\.json|h|hh|hpp|hxx|inc|ipp|c|cc|cpp|cxx)$"
)
WITH_DEBUG_SUFFIX = "_with_debug"
SETUP_CLANG_TIDY_BUILD_TARGETS = [
    "//:setup_clang_tidy",
    "//:clang_tidy_config",
    "//src/mongo/tools/mongo_tidy_checks:mongo_tidy_checks",
]
_WINDOWS_SYMLINKS_AVAILABLE = None


def _should_passthrough_target_name(target_name):
    return target_name.startswith(("install-", "archive-"))


def run_pty_command(cmd):
    stdout = None
    try:
        import pty

        parent_fd, child_fd = pty.openpty()  # provide tty
        stdout = ""

        proc = subprocess.Popen(cmd, stdout=child_fd, stdin=child_fd)
        os.close(child_fd)
        while True:
            try:
                data = os.read(parent_fd, 512)
            except OSError as e:
                if e.errno != errno.EIO:
                    raise
                break  # EIO means EOF on some systems
            else:
                if not data:  # EOF
                    break
            stdout += data.decode(errors="replace")
        returncode = proc.wait()
    except ModuleNotFoundError:
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        stdout = proc.stdout
        returncode = proc.returncode
        if returncode != 0:
            raise RuntimeError(
                f"Command failed (rc={returncode}): {' '.join(cmd)}\n"
                f"--- stdout ---\n{proc.stdout}\n"
                f"--- stderr ---\n{proc.stderr}"
            )
    if returncode != 0:
        raise RuntimeError(
            f"Command failed (rc={returncode}): {' '.join(cmd)}\n" f"--- output ---\n{stdout}"
        )
    return stdout


def _format_elapsed(reference_time):
    elapsed = time.monotonic() - reference_time
    if elapsed < 60:
        return f"{elapsed:.1f}s"

    minutes, seconds = divmod(elapsed, 60)
    if minutes < 60:
        return f"{int(minutes)}m {seconds:.1f}s"

    hours, minutes = divmod(minutes, 60)
    return f"{int(hours)}h {int(minutes)}m {seconds:.1f}s"


def _get_output_stream():
    for stream in [sys.stdout, sys.stderr, sys.__stderr__]:
        if not stream:
            continue
        try:
            if not stream.writable():
                continue
            return stream
        except (ValueError, OSError):
            continue
    return None


def _log_progress(message):
    line = f"[compiledb +{_format_elapsed(COMPILEDB_START_TIME)}] {message}"
    stream = _get_output_stream()
    if stream:
        try:
            print(line, file=stream, flush=True)
        except (ValueError, OSError):
            pass


def _run_build_command(cmd):
    """Run a bazel build, streaming output directly to the terminal.

    Uses a PTY so bazel sees a real terminal (colors, progress bar
    overwrites).  Both stdout and stderr are routed through the PTY
    and echoed to the wrapper's output stream so the output is visible
    even when tools/bazel has redirected the default fds to a log file.

    Falls back to a plain subprocess when no output stream is available
    or the ``pty`` module is missing (e.g. Windows).
    """

    stream = _get_output_stream()
    out_fd = None
    if stream:
        try:
            out_fd = stream.fileno()
        except (AttributeError, OSError):
            pass

    if out_fd is None:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        if proc.returncode != 0:
            raise RuntimeError(
                f"Command failed (rc={proc.returncode}): {' '.join(cmd)}\n"
                f"--- output ---\n{proc.stdout.decode(errors='replace')}"
            )
        return

    try:
        import pty

        parent_fd, child_fd = pty.openpty()
        proc = subprocess.Popen(cmd, stdout=child_fd, stderr=child_fd, stdin=child_fd)
        os.close(child_fd)
        captured = b""
        try:
            while True:
                try:
                    data = os.read(parent_fd, 4096)
                except OSError as e:
                    if e.errno != errno.EIO:
                        raise
                    break
                if not data:
                    break
                captured += data
                try:
                    os.write(out_fd, data)
                except OSError:
                    pass
        finally:
            os.close(parent_fd)
        returncode = proc.wait()
        stdout = captured.decode(errors="replace")
    except ModuleNotFoundError:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        captured = b""
        for chunk in iter(lambda: proc.stdout.read(4096), b""):
            captured += chunk
            try:
                os.write(out_fd, chunk)
            except OSError:
                pass
        returncode = proc.wait()
        stdout = captured.decode(errors="replace")

    if returncode != 0:
        raise RuntimeError(
            f"Command failed (rc={returncode}): {' '.join(cmd)}\n" f"--- output ---\n{stdout}"
        )


def clear_compiledb_posthook_state():
    try:
        os.remove(COMPILEDB_POSTHOOK_STATE)
    except OSError:
        pass


def _compiledb_build_settings(enterprise, atlas, log_default=False):
    compiledb_bazelrc = []
    compiledb_config = [COMPILEDB_BUILD_TAG_FILTERS]
    if (REPO_ROOT / ".bazelrc.compiledb").exists():
        compiledb_bazelrc = ["--bazelrc=.bazelrc", "--bazelrc=.bazelrc.compiledb"]
    else:
        if log_default:
            _log_progress(
                "No '.bazelrc.compiledb' found; using the Bazel invocation config for compiledb."
            )

    if not enterprise:
        compiledb_config.append("--build_enterprise=False")

    if not atlas:
        compiledb_config.append("--build_atlas=False")

    return compiledb_bazelrc, compiledb_config


def _resolve_compiledb_output_base(bazel_bin, persistent_compdb, startup_args=None):
    info_proc = subprocess.run(
        [bazel_bin] + list(startup_args or []) + ["info", "output_base"],
        capture_output=True,
        text=True,
    )
    if info_proc.returncode != 0:
        raise RuntimeError(
            f"Failed to query bazel output_base: rc={info_proc.returncode}\n"
            f"--- stdout ---\n{info_proc.stdout}\n"
            f"--- stderr ---\n{info_proc.stderr}"
        )

    symlink_prefix = None
    if persistent_compdb:
        output_base = pathlib.Path(info_proc.stdout.strip() + "_bazel_compiledb")
        os.makedirs(REPO_ROOT / ".compiledb", exist_ok=True)
        symlink_prefix = REPO_ROOT / ".compiledb" / "compiledb-"
    else:
        output_base = pathlib.Path(info_proc.stdout.strip())
    return output_base, symlink_prefix


def _compiledb_build_target(target):
    if target.endswith(WITH_DEBUG_SUFFIX):
        return target
    if "..." in target or "*" in target:
        return target
    if target.startswith("-"):
        return target
    if target.startswith("//") and ":" in target:
        package, name = target.rsplit(":", 1)
        if _should_passthrough_target_name(name):
            return target
        return f"{package}:{name}{WITH_DEBUG_SUFFIX}"
    if target.startswith(":"):
        if _should_passthrough_target_name(target[1:]):
            return target
        return target + WITH_DEBUG_SUFFIX
    if "/" not in target and ":" not in target:
        if _should_passthrough_target_name(target):
            return target
        return target + WITH_DEBUG_SUFFIX
    return target


def _resolve_compiledb_targets(target_scope_override=None, requested_targets=None):
    default_target_scope = "//src/..."
    if requested_targets:
        build_targets = [_compiledb_build_target(target) for target in requested_targets]
    else:
        scope = target_scope_override or os.environ.get(
            "MONGO_COMPILEDB_TARGET_SCOPE", default_target_scope
        )
        build_targets = [_compiledb_build_target(scope)]

    if build_targets != [default_target_scope]:
        _log_progress(f"Using compiledb target scope: {' '.join(build_targets)}")

    if len(build_targets) == 1:
        target_scope_expr = build_targets[0]
    else:
        target_scope_expr = "set(" + " ".join(build_targets) + ")"

    return default_target_scope, build_targets, target_scope_expr


def _resolve_extra_build_targets(extra_build_targets=None, setup_clang_tidy=False):
    resolved_targets = list(extra_build_targets or [])
    if setup_clang_tidy:
        for target in SETUP_CLANG_TIDY_BUILD_TARGETS:
            if target not in resolved_targets:
                resolved_targets.append(target)
    return resolved_targets


def _resolve_compiledb_flags(compiledb_config, requested_build_flags=None):
    build_flags = list(requested_build_flags or [])

    for default_flag in compiledb_config:
        if default_flag.startswith("--config="):
            if default_flag not in build_flags:
                build_flags.append(default_flag)
        elif default_flag.startswith("--build_enterprise="):
            if not any(arg.startswith("--build_enterprise=") for arg in build_flags):
                build_flags.append(default_flag)
        elif default_flag.startswith("--build_atlas="):
            if not any(arg.startswith("--build_atlas=") for arg in build_flags):
                build_flags.append(default_flag)
        elif default_flag not in build_flags:
            build_flags.append(default_flag)

    if not any(arg in ("--config=compiledb", "--config=compiledb-aspect") for arg in build_flags):
        build_flags.append("--config=compiledb")

    build_flags = [arg for arg in build_flags if not arg.startswith("--remote_download_regex=")]
    build_flags.append(f"--remote_download_regex={COMPILEDB_REQUIRED_OUTPUT_REGEX}")

    return build_flags


_EMBEDDED_ARG_OPTIONS = (
    "-include",
    "-imacros",
    "-include-pch",
    "-iquote",
    "-isystem",
    "-idirafter",
    "-iprefix",
    "-iwithprefix",
    "-iwithprefixbefore",
    "-isysroot",
    "-iframework",
    "-iframeworkwithsysroot",
    "--sysroot",
    "-Xclang",
    "-mllvm",
    "-target",
    "--target",
    "--gcc-toolchain",
    "-MF",
    "-MT",
    "-MQ",
    "-o",
    "-x",
)


def _split_embedded_arg_options(args):
    normalized = []
    for arg in args:
        split = False
        for option in _EMBEDDED_ARG_OPTIONS:
            prefix = option + " "
            if arg.startswith(prefix):
                normalized.extend([option, arg[len(prefix) :]])
                split = True
                break
        if not split:
            normalized.append(arg)
    return normalized


def _build_final_compile_command_entry(
    entry, arguments, repo_root_resolved, rewrite_exec_path, out_root_str, external_root_str
):
    compiledb_entry = {
        "file": rewrite_exec_path(entry["file"], out_root_str, external_root_str),
        "arguments": arguments,
        "directory": repo_root_resolved,
    }
    output_file = entry.get("output")
    if output_file:
        compiledb_entry["output"] = rewrite_exec_path(output_file, out_root_str, external_root_str)
    return compiledb_entry


def prepare_compiledb_posthook_args(
    bazel_bin,
    startup_args,
    command,
    build_flags,
    build_targets,
    persistent_compdb,
    enterprise,
    atlas,
    compiledb_targets=None,
    extra_build_targets=None,
    setup_clang_tidy=False,
):
    startup_args = list(startup_args)
    compiledb_targets = list(compiledb_targets or build_targets)
    extra_build_targets = list(extra_build_targets or [])
    owns_buildevents_path = False
    existing_output_base = next(
        (arg.split("=", 1)[1] for arg in startup_args if arg.startswith("--output_base=")),
        None,
    )
    existing_symlink_prefix = next(
        (arg.split("=", 1)[1] for arg in build_flags if arg.startswith("--symlink_prefix=")),
        None,
    )

    if existing_output_base:
        output_base = pathlib.Path(existing_output_base)
        symlink_prefix = pathlib.Path(existing_symlink_prefix) if existing_symlink_prefix else None
    else:
        output_base, symlink_prefix = _resolve_compiledb_output_base(
            bazel_bin,
            persistent_compdb,
            startup_args=startup_args,
        )
        if existing_symlink_prefix:
            symlink_prefix = pathlib.Path(existing_symlink_prefix)

    _, compiledb_config = _compiledb_build_settings(enterprise, atlas, log_default=True)
    build_flags = _resolve_compiledb_flags(compiledb_config, requested_build_flags=build_flags)

    if persistent_compdb and not any(arg.startswith("--output_base=") for arg in startup_args):
        startup_args.append(f"--output_base={output_base}")

    if (
        REPO_ROOT / ".bazelrc.compiledb"
    ).exists() and "--bazelrc=.bazelrc.compiledb" not in startup_args:
        startup_args.append("--bazelrc=.bazelrc.compiledb")

    if persistent_compdb and not any(arg.startswith("--symlink_prefix=") for arg in build_flags):
        build_flags.append(f"--symlink_prefix={symlink_prefix}")

    buildevents_path = None
    for arg in build_flags:
        if arg.startswith("--build_event_json_file="):
            buildevents_path = arg.split("=", 1)[1]
            break
    if not buildevents_path:
        with tempfile.NamedTemporaryFile(delete=False) as buildevents:
            buildevents_path = buildevents.name
        owns_buildevents_path = True
        build_flags.append(f"--build_event_json_file={buildevents_path}")

    os.makedirs(COMPILEDB_POSTHOOK_STATE.parent, exist_ok=True)
    with open(COMPILEDB_POSTHOOK_STATE, "w", encoding="utf-8") as state_file:
        json.dump(
            {
                "start_time": time.monotonic(),
                "persistent_compdb": persistent_compdb,
                "output_base": str(output_base),
                "symlink_prefix": str(symlink_prefix) if symlink_prefix else None,
                "build_flags": build_flags,
                "build_targets": compiledb_targets,
                "requested_targets": compiledb_targets,
                "setup_clang_tidy": setup_clang_tidy,
                "buildevents_path": buildevents_path,
                "delete_buildevents": owns_buildevents_path,
            },
            state_file,
        )

    return startup_args + [command] + build_flags + build_targets + extra_build_targets


def _artifact_exec_path(artifact, path_fragment_map):
    exec_path = artifact.get("execPath")
    if exec_path:
        return exec_path

    path_fragment_id = artifact.get("pathFragmentId")
    if not path_fragment_id:
        return None

    labels = []
    while path_fragment_id:
        fragment = path_fragment_map.get(path_fragment_id)
        if not fragment:
            return None
        labels.append(fragment["label"])
        path_fragment_id = fragment.get("parentId")

    labels.reverse()
    return "/".join(labels)


def _artifact_exec_path_by_id(
    artifact_id, artifact_map, path_fragment_map, artifact_exec_path_cache
):
    if artifact_id in artifact_exec_path_cache:
        return artifact_exec_path_cache[artifact_id]

    artifact = artifact_map.get(artifact_id)
    if not artifact:
        artifact_exec_path_cache[artifact_id] = None
        return None

    exec_path = _artifact_exec_path(artifact, path_fragment_map)
    artifact_exec_path_cache[artifact_id] = exec_path
    return exec_path


def _remove_existing_path(path):
    """Remove files, symlinks, and Windows junction-style directory entries safely."""
    try:
        path.unlink()
        return
    except FileNotFoundError:
        return
    except IsADirectoryError:
        pass
    except PermissionError:
        # Windows directory symlinks/junctions can land here instead of IsADirectoryError.
        pass

    if not os.path.lexists(path):
        return

    try:
        os.rmdir(path)
        return
    except OSError:
        pass

    shutil.rmtree(path)


def _windows_symlinks_available():
    global _WINDOWS_SYMLINKS_AVAILABLE

    if os.name != "nt":
        return True
    if _WINDOWS_SYMLINKS_AVAILABLE is not None:
        return _WINDOWS_SYMLINKS_AVAILABLE

    probe_root = pathlib.Path(tempfile.mkdtemp(prefix="compiledb-symlink-probe-"))
    probe_target = probe_root / "target"
    probe_link = probe_root / "link"
    probe_target.mkdir()

    try:
        os.symlink(probe_target.name, probe_link, target_is_directory=True)
        _WINDOWS_SYMLINKS_AVAILABLE = True
    except (NotImplementedError, OSError):
        _WINDOWS_SYMLINKS_AVAILABLE = False
    finally:
        _remove_existing_path(probe_link)
        shutil.rmtree(probe_root, ignore_errors=True)

    return _WINDOWS_SYMLINKS_AVAILABLE


def _copy_path(src, dst):
    if src.is_dir():
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def materialize_execroot_external_symlinks(output_base):
    external_root = output_base / "external"
    execroot_external = output_base / "execroot" / "_main" / "external"

    if not external_root.exists():
        return

    step_start = time.monotonic()
    execroot_external.mkdir(parents=True, exist_ok=True)
    created = 0
    updated = 0
    use_symlinks = _windows_symlinks_available()
    if not use_symlinks:
        _log_progress(
            "Symlink creation is unavailable; copying external repos into the compiledb execroot."
        )

    for repo in external_root.iterdir():
        link = execroot_external / repo.name
        link_target = os.path.relpath(repo, execroot_external)

        if os.path.lexists(link):
            if use_symlinks and link.is_symlink() and os.readlink(link) == link_target:
                continue
            if not use_symlinks and not link.is_symlink():
                _copy_path(repo, link)
                continue
            _remove_existing_path(link)
            updated += 1
        else:
            created += 1

        if use_symlinks:
            os.symlink(link_target, link, target_is_directory=repo.is_dir())
        else:
            _copy_path(repo, link)

    _log_progress(
        "Materialized execroot external repo symlinks "
        f"in {_format_elapsed(step_start)}: created={created} updated={updated}"
    )


def _exec_path_to_abs(output_base, path):
    path_obj = pathlib.Path(path)
    if path.startswith("external/"):
        return (output_base / "external" / path_obj.relative_to("external")).resolve(strict=False)
    return (output_base / "execroot" / "_main" / path_obj).resolve(strict=False)


def _collect_aspect_fragment_paths(
    bazel_bin,
    persistent_compdb,
    output_base,
    symlink_prefix,
    compiledb_bazelrc,
    compiledb_config,
    target_scope_expr,
    forwarded_startup_args=None,
):
    query_cmd = (
        [bazel_bin]
        + (
            forwarded_startup_args
            or ([f"--output_base={output_base}"] if persistent_compdb else [])
        )
        + compiledb_bazelrc
        + ["aquery"]
        + ([f"--symlink_prefix={symlink_prefix}"] if persistent_compdb else [])
        + compiledb_config
        + [
            "--bes_backend=",
            "--bes_results_url=",
            "--include_artifacts",
            f"deps({target_scope_expr})",
            "--output=jsonproto",
        ]
    )
    data = json.loads(run_pty_command(query_cmd))
    path_fragment_map = {fragment["id"]: fragment for fragment in data.get("pathFragments", [])}
    artifact_map = {artifact["id"]: artifact for artifact in data.get("artifacts", [])}
    artifact_exec_path_cache = {}

    fragment_paths = set()
    for action in data.get("actions", []):
        for artifact_id in action.get("outputIds", []):
            output_path = _artifact_exec_path_by_id(
                artifact_id,
                artifact_map,
                path_fragment_map,
                artifact_exec_path_cache,
            )
            if output_path and output_path.endswith(".compile_command.json"):
                fragment_paths.add(str(_exec_path_to_abs(output_base, output_path)))
    return sorted(fragment_paths)


def _generate_compiledb_via_aspect(
    bazel_bin,
    persistent_compdb,
    enterprise,
    atlas,
    target_scope_override=None,
    requested_build_flags=None,
    requested_targets=None,
    extra_build_targets=None,
    setup_clang_tidy=False,
    startup_args=None,
    prepared_output_base=None,
    prepared_symlink_prefix=None,
    prepared_buildevents_path=None,
    delete_buildevents=True,
    skip_build=False,
):
    write_wrapper_hook_bazelrc([])

    def rewrite_exec_path(path, out_root_str, external_root_str):
        if not path:
            return path
        if path.startswith("bazel-out/"):
            return out_root_str + "/" + path[len("bazel-out/") :]
        if path.startswith("external/"):
            return external_root_str + "/" + path[len("external/") :]
        return path

    def rewrite_args(args, out_root_str, external_root_str):
        def rewrite_arg_path(arg):
            if out_root_str and arg.startswith("bazel-out/"):
                return out_root_str + "/" + arg[len("bazel-out/") :]
            if external_root_str and arg.startswith("external/"):
                return external_root_str + "/" + arg[len("external/") :]

            # Some toolchain flags embed execroot-relative paths as option=value, e.g.
            # "-fprofile-use=external/.../clang_pgo.profdata".
            if "=" in arg:
                prefix, value = arg.split("=", 1)
                rewritten_value = rewrite_arg_path(value)
                if rewritten_value != value:
                    return f"{prefix}={rewritten_value}"

            return arg

        args = _split_embedded_arg_options(args)
        rewritten = []
        for arg in args:
            rewritten_arg = rewrite_arg_path(arg)
            if rewritten_arg != arg:
                arg = rewritten_arg
            else:
                # Preserve compiler prefixes while rewriting paths.
                m = re.match(r"^(/external:I)(bazel-out|external)/(.*)$", arg)
                if m:
                    prefix, root, rest = m.groups()
                    if root == "bazel-out" and out_root_str:
                        arg = f"{prefix}{out_root_str}/{rest}"
                    elif root == "external" and external_root_str:
                        arg = f"{prefix}{external_root_str}/{rest}"
                else:
                    m = re.match(r"^(-isystem)(bazel-out|external)/(.*)$", arg)
                    if m:
                        prefix, root, rest = m.groups()
                        if root == "bazel-out" and out_root_str:
                            arg = f"{prefix}{out_root_str}/{rest}"
                        elif root == "external" and external_root_str:
                            arg = f"{prefix}{external_root_str}/{rest}"
                    else:
                        # Generic: preserve any two-character prefix (e.g. "-I", "/I").
                        m = re.match(r"^(.{2})(bazel-out|external)/(.*)$", arg)
                        if m:
                            prefix, root, rest = m.groups()
                            if root == "bazel-out" and out_root_str:
                                arg = f"{prefix}{out_root_str}/{rest}"
                            elif root == "external" and external_root_str:
                                arg = f"{prefix}{external_root_str}/{rest}"
            rewritten.append(arg)
        return rewritten

    def with_librdkafka_config_header(args, input_file, out_root_str):
        if "src/third_party/private/librdkafka/dist/src/" not in input_file and (
            "src/third_party/private/librdkafka/dist/src-cpp/" not in input_file
        ):
            return args

        out_bin_root = None
        for arg in args:
            if "/bazel-out/" in arg and arg.endswith("/bin"):
                out_bin_root = pathlib.Path(arg)
                break

        if out_bin_root:
            config_header = (
                out_bin_root
                / "src"
                / "third_party"
                / "private"
                / "librdkafka"
                / "dist"
                / "FAKE"
                / "config.h"
            )
        else:
            config_header = (
                pathlib.Path(out_root_str)
                / "src"
                / "third_party"
                / "private"
                / "librdkafka"
                / "dist"
                / "FAKE"
                / "config.h"
            )
        config_header_str = config_header.as_posix()
        if any(arg == config_header_str for arg in args):
            return args

        rewritten = list(args)
        try:
            c_index = next(i for i, arg in enumerate(rewritten) if arg in ("-c", "/c"))
            rewritten[c_index:c_index] = ["-include", config_header_str]
        except StopIteration:
            rewritten.extend(["-include", config_header_str])
        return rewritten

    def setup_clang_tidy_from_built_outputs():
        candidate_bin_dirs = []
        if persistent_compdb and symlink_prefix:
            candidate_bin_dirs.append(pathlib.Path(f"{symlink_prefix}bin"))
        candidate_bin_dirs.append(REPO_ROOT / "bazel-bin")

        config_src = None
        plugin_src = None
        for bin_dir in candidate_bin_dirs:
            config_candidate = bin_dir / ".clang-tidy"
            plugin_dir = bin_dir / "src" / "mongo" / "tools" / "mongo_tidy_checks"
            plugin_candidate = next(
                (
                    plugin_dir / candidate
                    for candidate in PLUGIN_CANDIDATES
                    if (plugin_dir / candidate).exists()
                ),
                None,
            )
            if config_candidate.exists() and plugin_candidate:
                config_src = config_candidate
                plugin_src = plugin_candidate
                break

        if not config_src:
            _log_progress(
                "Skipping clang-tidy IDE setup because the .clang-tidy output is unavailable."
            )
            return
        if not plugin_src:
            _log_progress(
                "Skipping clang-tidy IDE setup because the mongo_tidy_checks plugin output is unavailable."
            )
            return

        materialize_clang_tidy_ide_files(REPO_ROOT, config_src, plugin_src)
        _log_progress("Set up clang-tidy IDE integration files.")

    output_base = pathlib.Path(prepared_output_base) if prepared_output_base else None
    symlink_prefix = pathlib.Path(prepared_symlink_prefix) if prepared_symlink_prefix else None
    if output_base is None:
        output_base, symlink_prefix = _resolve_compiledb_output_base(
            bazel_bin,
            persistent_compdb,
            startup_args=startup_args,
        )

    real_out_root = pathlib.Path(os.path.realpath(output_base / "execroot" / "_main" / "bazel-out"))
    real_external_root = pathlib.Path(os.path.realpath(output_base / "external"))
    real_out_root_str = real_out_root.as_posix()
    real_external_root_str = real_external_root.as_posix()

    compiledb_bazelrc, compiledb_config = _compiledb_build_settings(
        enterprise,
        atlas,
        log_default=not skip_build,
    )
    default_target_scope, build_targets, target_scope_expr = _resolve_compiledb_targets(
        target_scope_override=target_scope_override,
        requested_targets=requested_targets,
    )
    extra_build_targets = _resolve_extra_build_targets(
        extra_build_targets=extra_build_targets,
        setup_clang_tidy=setup_clang_tidy,
    )
    build_flags = _resolve_compiledb_flags(
        compiledb_config,
        requested_build_flags=requested_build_flags,
    )
    if prepared_buildevents_path:
        buildevents_path = prepared_buildevents_path
    else:
        with tempfile.NamedTemporaryFile(delete=False) as buildevents:
            buildevents_path = buildevents.name

    # Build the startup args for the compiledb build invocation.
    # When persistent_compdb, we use a dedicated output_base (overrides any
    # --output_user_root in the original startup_args, which is fine).
    # Otherwise, forward the caller's startup_args so the build resolves to
    # the same output tree (e.g. CI passes --output_user_root).
    forwarded_startup_args = list(startup_args or [])
    if persistent_compdb:
        forwarded_startup_args = [
            arg for arg in forwarded_startup_args if not arg.startswith("--output_base=")
        ]
        forwarded_startup_args.append(f"--output_base={output_base}")

    try:
        if not skip_build:
            build_start = time.monotonic()
            _log_progress("Generating compiledb command fragments via aspect...")
            build_cmd = (
                [bazel_bin]
                + forwarded_startup_args
                + compiledb_bazelrc
                + ["build"]
                + ([f"--symlink_prefix={symlink_prefix}"] if persistent_compdb else [])
                + build_flags
                + [
                    f"--build_event_json_file={buildevents_path}",
                ]
                + build_targets
            )
            _run_build_command(build_cmd)
            _log_progress(
                "Generated compiledb command fragments via aspect "
                f"in {_format_elapsed(build_start)}"
            )

        materialize_execroot_external_symlinks(output_base)

        load_start = time.monotonic()
        raw_entries = load_compile_command_fragments(
            buildevents_path,
            output_base=output_base,
        )
        if not raw_entries:
            fragment_paths = _collect_aspect_fragment_paths(
                bazel_bin=bazel_bin,
                persistent_compdb=persistent_compdb,
                output_base=output_base,
                symlink_prefix=symlink_prefix if persistent_compdb else None,
                compiledb_bazelrc=compiledb_bazelrc,
                compiledb_config=build_flags,
                target_scope_expr=target_scope_expr,
                forwarded_startup_args=forwarded_startup_args,
            )
            raw_entries = load_compile_command_fragments_from_paths(fragment_paths)
        if not raw_entries:
            raise RuntimeError(
                "No compile command fragments were produced by the compiledb aspect."
            )
        _log_progress(
            "Loaded compiledb command fragments "
            f"in {_format_elapsed(load_start)}: {len(raw_entries)} fragment(s)"
        )

        repo_root_resolved = str(REPO_ROOT.resolve())
        output_json = []
        for entry in raw_entries:
            input_file = entry["file"]
            args = rewrite_args(entry["arguments"], real_out_root_str, real_external_root_str)
            args = with_librdkafka_config_header(args, input_file, real_out_root_str)
            output_json.append(
                _build_final_compile_command_entry(
                    entry,
                    args,
                    repo_root_resolved,
                    rewrite_exec_path,
                    real_out_root_str,
                    real_external_root_str,
                )
            )

        output_json.sort(key=compile_command_sort_key)
        write_compile_commands(output_json, REPO_ROOT / "compile_commands.json")
        if setup_clang_tidy and extra_build_targets:
            _log_progress("Building clang-tidy IDE targets...")
            tidy_build_cmd = (
                [bazel_bin]
                + forwarded_startup_args
                + compiledb_bazelrc
                + ["build"]
                + ([f"--symlink_prefix={symlink_prefix}"] if persistent_compdb else [])
                + extra_build_targets
            )
            try:
                _run_build_command(tidy_build_cmd)
            except RuntimeError:
                _log_progress(
                    "Warning: failed to build clang-tidy targets; " "skipping clang-tidy IDE setup."
                )
                setup_clang_tidy = False
        if setup_clang_tidy:
            setup_clang_tidy_from_built_outputs()
    finally:
        if delete_buildevents:
            try:
                os.remove(buildevents_path)
            except OSError:
                pass

    if persistent_compdb:
        shutdown_proc = subprocess.run(
            [bazel_bin, f"--output_base={output_base}", "shutdown"], capture_output=True, text=True
        )
        if shutdown_proc.returncode != 0:
            _log_progress(f"Failed to shutdown compiledb output_base: {shutdown_proc.returncode}")
            _log_progress("--- stdout ---")
            print(shutdown_proc.stdout)
            _log_progress("--- stderr ---")
            print(shutdown_proc.stderr)

    _log_progress("compiledb target done, finishing any other targets...")


def finalize_compiledb_posthook(bazel_bin, enterprise, atlas):
    global COMPILEDB_START_TIME

    if platform.system() == "Windows":
        return
    if not COMPILEDB_POSTHOOK_STATE.exists():
        return

    with open(COMPILEDB_POSTHOOK_STATE, "r", encoding="utf-8") as state_file:
        state = json.load(state_file)

    COMPILEDB_START_TIME = state.get("start_time", COMPILEDB_START_TIME)

    try:
        _generate_compiledb_via_aspect(
            bazel_bin=bazel_bin,
            persistent_compdb=state["persistent_compdb"],
            enterprise=enterprise,
            atlas=atlas,
            requested_build_flags=state["build_flags"],
            requested_targets=state.get("requested_targets", state["build_targets"]),
            setup_clang_tidy=state.get("setup_clang_tidy", False),
            prepared_output_base=state["output_base"],
            prepared_symlink_prefix=state.get("symlink_prefix"),
            prepared_buildevents_path=state["buildevents_path"],
            delete_buildevents=state.get("delete_buildevents", False),
            skip_build=True,
        )
    finally:
        clear_compiledb_posthook_state()


def generate_compiledb(
    bazel_bin,
    persistent_compdb,
    enterprise,
    atlas,
    target_scope_override=None,
    requested_build_flags=None,
    requested_targets=None,
    extra_build_targets=None,
    setup_clang_tidy=False,
    startup_args=None,
):
    return _generate_compiledb_via_aspect(
        bazel_bin=bazel_bin,
        persistent_compdb=persistent_compdb,
        enterprise=enterprise,
        atlas=atlas,
        target_scope_override=target_scope_override,
        requested_build_flags=requested_build_flags,
        requested_targets=requested_targets,
        extra_build_targets=extra_build_targets,
        setup_clang_tidy=setup_clang_tidy,
        startup_args=startup_args,
    )
