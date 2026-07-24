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

    if install_modules(bazel, args):
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
    group).

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
        uv_sync_sh = REPO_ROOT / "buildscripts" / "uv_sync.sh"
        if not uv_sync_sh.exists():
            # This shouldn't happen inside a mongo repo, but if it does — fall
            # through and let the wrapper's first missing-import fail loudly
            # with a real ImportError.
            return False
        proc = subprocess.run(["bash", str(uv_sync_sh), "-f"], cwd=str(REPO_ROOT))
        synced = proc.returncode == 0
        venv_root = _target_venv()
        stamp = venv_root / _STAMP_NAME

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
