import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).parent.parent.parent
sys.path.append(str(REPO_ROOT))

from bazel.wrapper_hook.wrapper_debug import wrapper_debug

MODULES_READY_ENV = "MONGO_BAZEL_WRAPPER_MODULES_READY"

# The wrapper hook's third-party deps are declared as the `wrapper-hook`
# PEP 735 dependency group in //pyproject.toml — that group is the single
# source of truth for the package list. Bootstrap is a targeted
# `uv sync --only-group wrapper-hook`:
#   --locked   uv.lock must already satisfy pyproject.toml (never rewrite it)
#   --inexact  additive: never prune packages other groups installed into
#              the same venv (a bare sync would strip a fully-populated
#              dev venv down to just this group)
_UV_SYNC_ARGS = [
    "sync",
    "--only-group",
    "wrapper-hook",
    "--locked",
    "--inexact",
    # `--only-group` already omits the project; this just silences uv's
    # entry-points warning about the unpackaged root project.
    "--no-install-project",
]

# Name of the stamp file written into the target venv after a successful
# bootstrap sync. Contains the sha256 of uv.lock at sync time; when it
# matches the current uv.lock the sync is skipped entirely, keeping the
# per-invocation overhead to one small file read.
_STAMP_NAME = ".wrapper_hook_uv_stamp"

# Cygwin-style path prefix: `/cygdrive/<letter>/...`. Evergreen's Windows
# hosts run bash steps under Cygwin/Git-Bash and export `$VIRTUAL_ENV`
# in that form (e.g. `/cygdrive/c/data/mci/0f83/venv`). The wrapper runs
# under Windows-native Python (`py_host/dist/python.exe`), which needs
# `C:\data\mci\0f83\venv` to resolve the same path.
_CYGDRIVE_RE = re.compile(r"^/cygdrive/([a-zA-Z])(/.*)?$")
_WINDOWS_BASH_SYSTEM_PREFIXES = ("CYGWIN_NT", "MINGW", "MSYS")
_BASH_OVERRIDE_ENV = "MONGO_BAZEL_BASH"

# The hermetic Python toolchain repo, as materialized under the output base by
# `@py_host//:all`. tools/bazel and tools/bazel.bat locate the wrapper
# interpreter the same way; keep the three in sync.
_PY_HOST_REPO_DIR = "_main~setup_mongo_python_toolchains~py_host"
# tools/bazel.bat copies the py_host dist here when it can't run the
# interpreter in place (Windows file locking), so the copy is hermetic too.
_WINDOWS_WRAPPER_PYTHON_PARTS = (".tmp", "bazel", "wrapper-python")
# Minimum interpreter version the repo's Python code requires; mirrors the
# `python_works` check in tools/bazel.
_MIN_PYTHON_VERSION = (3, 13)


class MixedPlatformError(RuntimeError):
    """Raised when native Windows tooling is paired with a POSIX environment."""


def _windows_bash_candidates() -> list[str]:
    """Return likely Bash executables, including native Bash behind WSL on PATH."""
    candidates: list[str] = []
    seen: set[str] = set()

    def add(candidate: str | os.PathLike[str] | None) -> None:
        if not candidate:
            return
        candidate = str(candidate)
        key = candidate.casefold()
        if key not in seen:
            seen.add(key)
            candidates.append(candidate)

    add(os.environ.get(_BASH_OVERRIDE_ENV))
    add(shutil.which("bash"))
    add(shutil.which("bash.exe"))

    # If WSL's bash.exe wins PATH lookup, inspect the other PATH entries too.
    for path_entry in os.environ.get("PATH", "").split(os.pathsep):
        if path_entry:
            bash_path = pathlib.Path(path_entry) / "bash.exe"
            if bash_path.is_file():
                add(bash_path)

    git_executable = shutil.which("git.exe") or shutil.which("git")
    if git_executable:
        git_path = pathlib.Path(git_executable)
        if git_path.parent.name.casefold() in {"cmd", "bin"}:
            git_root = git_path.parent.parent
            add(git_root / "bin" / "bash.exe")
            add(git_root / "usr" / "bin" / "bash.exe")

    for root_variable in ("ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"):
        root = os.environ.get(root_variable)
        if root:
            git_roots = [pathlib.Path(root) / "Git"]
            if root_variable == "LOCALAPPDATA":
                git_roots.append(pathlib.Path(root) / "Programs" / "Git")
            for git_root in git_roots:
                add(git_root / "bin" / "bash.exe")
                add(git_root / "usr" / "bin" / "bash.exe")

    system_drive = os.environ.get("SystemDrive", "C:")
    for cygwin_root in (
        pathlib.Path(system_drive) / "cygwin64",
        pathlib.Path(system_drive) / "cygwin",
    ):
        add(cygwin_root / "bin" / "bash.exe")

    return candidates


def _check_windows_bash(repo_root: pathlib.Path) -> str:
    """Find compatible native Bash, skipping WSL, when running Windows Python."""
    if os.name != "nt":
        return "bash"

    detected_systems = []
    wsl_detected = False
    for bash in _windows_bash_candidates():
        try:
            result = subprocess.run(
                [bash, "-c", "uname -s"],
                cwd=str(repo_root),
                capture_output=True,
                text=True,
            )
        except OSError:
            continue

        bash_system = result.stdout.strip()
        normalized_system = bash_system.upper()
        if result.returncode == 0 and normalized_system.startswith(_WINDOWS_BASH_SYSTEM_PREFIXES):
            return bash
        if bash_system:
            detected_systems.append(f"{bash}: {bash_system}")
            wsl_detected = wsl_detected or normalized_system.startswith("LINUX")

    detected = ", ".join(detected_systems) or "no usable bash.exe found"
    if wsl_detected:
        raise MixedPlatformError(
            "The Windows Bazel wrapper found WSL Bash but no compatible Git Bash "
            f"or Cygwin. Detected: {detected}. Install Git Bash/Cygwin or set "
            f"{_BASH_OVERRIDE_ENV} to a native bash.exe path."
        )
    raise MixedPlatformError(
        "The Windows Bazel wrapper could not find Git Bash or Cygwin. "
        f"Detected: {detected}. Install Git Bash/Cygwin or set "
        f"{_BASH_OVERRIDE_ENV} to a native bash.exe path."
    )


def _windows_path_for_bash(path: str) -> str:
    """Make a native Windows path usable as a command in Git Bash/Cygwin."""
    return path.replace("\\", "/")


def _check_venv_layout(venv_root: pathlib.Path) -> None:
    """Reject a venv created for the opposite host platform."""
    if os.name != "nt" or _venv_python(venv_root).exists():
        return

    posix_python = venv_root / "bin" / "python3"
    if posix_python.exists():
        raise MixedPlatformError(
            f"The venv at {venv_root.resolve()} is a POSIX/WSL venv, but the "
            "wrapper is running under native Windows Python. Rename or remove "
            "that venv, then rerun from Git Bash or Cygwin."
        )


def _from_cygwin_path(p):
    """Translate a Cygwin-style `/cygdrive/<letter>/...` path to native
    Windows form when we're running under Windows Python. On non-Windows
    hosts, return the input unchanged.
    """
    if os.name != "nt" or not p:
        return p
    m = _CYGDRIVE_RE.match(str(p))
    if not m:
        return p
    drive_letter = m.group(1).upper()
    tail = (m.group(2) or "").replace("/", "\\")
    return f"{drive_letter}:{tail}"


def _target_venv():
    """The venv the wrapper syncs into and imports from: the active venv
    pointed at by $VIRTUAL_ENV (Evergreen activates `${workdir}/venv` via
    `prelude_venv.sh::activate_venv` before invoking bazel), else the
    workstation `python3-venv` at the repo root.

    A $VIRTUAL_ENV that doesn't actually contain an interpreter (stale env
    var, venv deleted out from under it) is ignored so the bootstrap and
    sys.path setup agree on the python3-venv fallback.
    """
    active_venv = os.environ.get("VIRTUAL_ENV")
    if active_venv:
        venv_root = pathlib.Path(_from_cygwin_path(active_venv))
        if _venv_python(venv_root).exists():
            return venv_root
        wrapper_debug(f"$VIRTUAL_ENV={active_venv} has no interpreter; falling back")
    return REPO_ROOT / "python3-venv"


def _py_host_interpreter_relpath() -> pathlib.Path:
    """Interpreter path within the py_host repo's `dist` tree. The Windows
    CPython distribution puts python.exe at the root of the dist; the POSIX
    ones use the usual `bin/` prefix.
    """
    if os.name == "nt":
        return pathlib.Path("dist", "python.exe")
    return pathlib.Path("dist", "bin", "python3")


def _is_hermetic_python(candidate) -> bool:
    """True if `candidate` is an interpreter from the hermetic toolchain —
    either in the py_host repo tree under the output base, or the copy of
    that tree tools/bazel.bat stages under `.tmp/bazel/wrapper-python`.
    """
    if not candidate:
        return False
    parts = pathlib.PurePath(candidate).parts
    if _PY_HOST_REPO_DIR in parts:
        return True
    n = len(_WINDOWS_WRAPPER_PYTHON_PARTS)
    return any(parts[i : i + n] == _WINDOWS_WRAPPER_PYTHON_PARTS for i in range(len(parts)))


def _python_works(candidate) -> bool:
    """Mirror of tools/bazel's `python_works`: an executable interpreter new
    enough to run this repo's Python.
    """
    candidate = pathlib.Path(candidate)
    if not candidate.is_file() or not os.access(candidate, os.X_OK):
        return False
    try:
        proc = subprocess.run(
            [
                str(candidate),
                "-c",
                "import sys; raise SystemExit(0 if sys.version_info >= "
                f"{_MIN_PYTHON_VERSION} else 1)",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return False
    return proc.returncode == 0


def _hermetic_python():
    """Path to the hermetic toolchain interpreter, or None if it isn't
    materialized under this repo's output base yet.

    The venv the wrapper hook bootstraps is created by
    `<interpreter> -m venv`, so it inherits whichever interpreter we hand
    `uv_sync.sh`. Using the Bazel-managed Python keeps that venv on the same
    hermetic CPython the rest of the build uses, instead of whatever
    `python3` happens to be first on PATH.

    Two sources, cheapest first:
      1. The running interpreter, when the wrapper was itself launched from
         the toolchain (the normal path — tools/bazel and tools/bazel.bat
         both prefer it). No probing needed.
      2. The py_host tree reached through Bazel's convenience symlink
         (or the `.compiledb` one, for compiledb-only output bases).
    """
    if _is_hermetic_python(sys.executable):
        return pathlib.Path(sys.executable)

    repo_root = REPO_ROOT.resolve()
    symlinks = [
        repo_root / f"bazel-{repo_root.name}",
        repo_root / ".compiledb" / f"compiledb-{repo_root.name}",
    ]
    for symlink in symlinks:
        if not symlink.exists():
            continue
        # <output_base>/execroot/_main/../../external/<py_host>/dist/...
        candidate = (
            symlink.resolve().parent.parent
            / "external"
            / _PY_HOST_REPO_DIR
            / _py_host_interpreter_relpath()
        )
        if _python_works(candidate):
            wrapper_debug(f"using hermetic python for bootstrap: {candidate}")
            return candidate

    wrapper_debug("hermetic python toolchain not found; bootstrapping without it")
    return None


def _venv_python(venv_root):
    if os.name == "nt":
        return venv_root / "Scripts" / "python.exe"
    return venv_root / "bin" / "python3"


def _venv_site_packages(venv_root):
    """Yield site-packages directories under a venv root, or nothing if
    `venv_root` isn't a venv-shaped tree.

    Two layouts to handle:
      - Unix: `<venv>/lib/python<major>.<minor>/site-packages/` (site-packages
        sits under a per-version subdir).
      - Windows: `<venv>/Lib/site-packages/` (site-packages sits directly
        under Lib — no per-version subdir).

    We can't dispatch by testing `.exists()` on `lib` vs `Lib` because
    Windows is case-insensitive — `venv / "lib"` matches `Lib` on Windows,
    which would send us down the Unix branch by mistake. Instead we probe
    for both site-packages patterns unconditionally under the resolved
    lib dir (matched case-insensitively) and yield whichever exists.
    """
    if venv_root is None or not venv_root.exists():
        return

    lib_dir = venv_root / "lib"
    if not lib_dir.exists():
        lib_dir = venv_root / "Lib"
    if not lib_dir.exists():
        return

    # Windows layout: site-packages directly under Lib.
    direct = lib_dir / "site-packages"
    if direct.exists():
        yield direct

    # Unix layout: site-packages under pythonX.Y/ subdirs. Both branches
    # may fire on the rare Unix venv that also has a stray Lib/site-packages
    # sibling, which is harmless — callers iterate all yielded paths.
    for entry in lib_dir.iterdir():
        if entry.name == "site-packages":
            continue  # Already yielded above.
        sp = entry / "site-packages"
        if sp.exists():
            yield sp


def setup_python_path():
    """Append the target venv's site-packages to sys.path so that
    subsequent imports in the wrapper hook resolve.

    install_modules() guarantees the venv satisfies the `wrapper-hook`
    dependency group before this runs, so the venv is the only package
    source needed. Appending (not prepending) keeps the running
    interpreter's stdlib first.
    """
    for sp in _venv_site_packages(_target_venv()):
        sys.path.append(str(sp))


def _uv_lock_hash():
    lock_file = REPO_ROOT / "uv.lock"
    if not lock_file.exists():
        return None
    return hashlib.sha256(lock_file.read_bytes()).hexdigest()


def _run_uv_sync(venv_root):
    """Run the targeted wrapper-hook group sync into `venv_root` with the
    first available uv. Returns True on success.

    uv candidates, in order:
      1. The venv's own interpreter's uv module (uv is in the lock's
         `export` group, so any venv populated by uv_sync.sh/venv_setup.sh
         has it).
      2. A `uv` binary on PATH (workstations that installed uv via pipx).
    """
    candidates = []
    venv_py = _venv_python(venv_root)
    if venv_py.exists():
        candidates.append([str(venv_py), "-m", "uv"])
    uv_on_path = shutil.which("uv")
    if uv_on_path:
        candidates.append([uv_on_path])

    env = dict(os.environ, UV_PROJECT_ENVIRONMENT=str(venv_root))
    for uv_cmd in candidates:
        cmd = uv_cmd + _UV_SYNC_ARGS
        wrapper_debug(f"wrapper bootstrap: {' '.join(cmd)}")
        proc = subprocess.run(cmd, cwd=str(REPO_ROOT), env=env)
        if proc.returncode == 0:
            return True
        wrapper_debug(f"wrapper bootstrap sync failed (exit {proc.returncode}); trying next uv")
    return False


def _reexec_current_python(env_var: str = MODULES_READY_ENV) -> None:
    wrapper_debug("python deps changed; restarting wrapper interpreter")
    env = os.environ.copy()
    env[env_var] = "1"
    if os.name == "nt":
        # os.execve on Windows spawns a new process and immediately exits the
        # current one; tools/bazel.bat then reads MONGO_BAZEL_WRAPPER_ARGS
        # before the new process has written it.  subprocess.run keeps the
        # current process alive until the child finishes, so bazel.bat reads
        # the file only after the child has written the correct args.
        result = subprocess.run([sys.executable, *sys.argv], env=env)
        sys.exit(result.returncode)
    os.execve(sys.executable, [sys.executable, *sys.argv], env)


def bootstrap_modules(bazel, args):
    # Nested Bazel installs can refresh the repo-rule python tree under the
    # running interpreter. Re-exec so later stdlib imports come from the
    # refreshed tree instead of the potentially stale one this process started
    # with.
    if os.environ.get(MODULES_READY_ENV) == "1":
        setup_python_path()
        return

    try:
        modules_installed = install_modules(bazel, args)
    except MixedPlatformError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1) from None

    if modules_installed:
        _reexec_current_python()
    else:
        setup_python_path()


def install_modules(bazel, args):
    """Ensure the wrapper hook's python deps (the `wrapper-hook` dependency
    group in //pyproject.toml) are installed in the target venv.

    The check-and-install step is a single idempotent
    `uv sync --only-group wrapper-hook --locked --inexact`, skipped when a
    stamp file in the venv already records the current uv.lock hash. If no
    venv exists at all (fresh host), `buildscripts/uv_sync.sh -f` bootstraps
    `python3-venv` from scratch (creating the venv, installing pinned uv
    into it, and running a full `--all-groups` sync, which includes this
    group) using the hermetic toolchain interpreter.

    Notably, we do NOT try to materialize wheels via
    `bazel build @pypi//:<pkg>`. That path used to be here (matching the
    zac/uv-poc pip.parse implementation's approach), but under
    rules_pycross the wheel_installer.exe launcher fails with
    STATUS_DLL_NOT_FOUND on Windows exec-config due to a
    rules_python+Bazel Windows launcher / Python DLL loading issue that
    we don't own. Routing through uv sidesteps that entire launcher path —
    uv is a self-contained Rust binary that doesn't need Bazel's py_binary
    infrastructure — and uniformly works on all three platforms.

    Returns True if the venv was modified (caller must re-exec so the
    fresh venv is scanned), False if no work was needed.
    """
    venv_root = _target_venv()
    _check_venv_layout(venv_root)
    lock_hash = _uv_lock_hash()
    stamp = venv_root / _STAMP_NAME

    if lock_hash is not None and stamp.exists():
        try:
            if stamp.read_text(encoding="utf-8").strip() == lock_hash:
                wrapper_debug("wrapper deps in sync with uv.lock (stamp match); skipping")
                return False
        except OSError:
            pass

    if _venv_python(venv_root).exists():
        synced = _run_uv_sync(venv_root)
    else:
        # Fresh host: no venv anywhere. uv_sync.sh -f creates python3-venv,
        # installs the pinned uv into it, and runs the full sync. Invoke via
        # bash so this works uniformly on Linux/macOS + Windows (Evergreen
        # Windows hosts have Git Bash / Cygwin on PATH).
        wrapper_debug("no venv found; bootstrapping python3-venv via uv_sync.sh -f")
        repo_root = REPO_ROOT.resolve()
        uv_sync_sh = repo_root / "buildscripts" / "uv_sync.sh"
        if not uv_sync_sh.exists():
            # This shouldn't happen inside a mongo repo, but if it does — fall
            # through and let the wrapper's first missing-import fail loudly
            # with a real ImportError.
            return False
        bash_command = _check_windows_bash(repo_root)
        # Pass a relative path so native Windows Python never hands a
        # Windows- or Cygwin-style absolute path to Bash. Bash resolves the
        # path from the native working directory set above.
        bootstrap_env = os.environ.copy()
        # _target_venv() deliberately ignores stale or opposite-platform
        # VIRTUAL_ENV values. Do the same for uv_sync.sh, whose `-f` mode
        # otherwise gives an inherited VIRTUAL_ENV precedence over the
        # workstation python3-venv fallback.
        bootstrap_env.pop("VIRTUAL_ENV", None)
        # uv_sync.sh creates python3-venv with `$PYTHON3 -m venv`. Pin that to
        # the hermetic toolchain interpreter so the wrapper's venv matches the
        # Python the build uses, rather than an arbitrary system python3 that
        # may be the wrong version or missing ensurepip.
        bootstrap_python = _hermetic_python()
        if bootstrap_python is None and os.name == "nt":
            # No py_host tree yet. A native Windows wrapper may be launched
            # while WSL's python3 is first on PATH, so fall back to the
            # running interpreter to keep the bootstrap on the same side of
            # the platform boundary as the wrapper itself.
            bootstrap_python = pathlib.Path(sys.executable)
        if bootstrap_python is not None:
            bootstrap_env["PYTHON3"] = (
                _windows_path_for_bash(str(bootstrap_python))
                if os.name == "nt"
                else str(bootstrap_python)
            )
        proc = subprocess.run(
            [bash_command, "buildscripts/uv_sync.sh", "-f"],
            cwd=str(repo_root),
            env=bootstrap_env,
        )
        synced = proc.returncode == 0
        venv_root = _target_venv()
        stamp = venv_root / _STAMP_NAME
        if synced and not _venv_python(venv_root).exists():
            _check_venv_layout(venv_root)
            raise MixedPlatformError(
                f"Bash bootstrap completed, but the expected interpreter was not "
                f"created at {_venv_python(venv_root)}."
            )

    if not synced:
        print(
            "Warning: failed to sync the `wrapper-hook` dependency group into "
            f"{venv_root}. The bazel wrapper hook may fail on its next import; "
            "run `bash buildscripts/uv_sync.sh` to repair the venv.",
            file=sys.stderr,
        )
        return False

    if lock_hash is not None:
        try:
            stamp.write_text(lock_hash + "\n", encoding="utf-8")
        except OSError as exc:
            wrapper_debug(f"could not write wrapper bootstrap stamp: {exc}")

    return True
