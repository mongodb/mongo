#!/bin/bash
# Run this in a mongo git repo to install the Python dependencies needed to
# build/test the repo, as specified in `uv.lock`.
#
# If your virtual env has been activated, you can usually just execute this
# script with no arguments.
#
# Optionally override the Python interpreter with `$PYTHON3`, or with the
# `-p <path_to_python3>` option. It defaults to `python3`, which is usually
# correct.
#
# Normally, this script refuses to install unless a virtual env is active.
# This avoids making accidental system-wide changes. A user can override this
# safety measure with `-f`.
#
# This is the uv-era replacement for `buildscripts/poetry_sync.sh`. uv is
# pinned to a known-good version and installed into the active venv if it
# isn't already there. The actual dependency install is `uv sync --all-groups
# --no-install-project` (equivalent to the prior `poetry sync --no-root`).

# uv version pin — canonical source is buildscripts/uv_version.txt.
# All uv installers in this repo (venv_setup.sh, set_up_workstation.sh,
# .devcontainer/Dockerfile, buildscripts/antithesis/.../Dockerfile,
# buildscripts/resmokelib/powercycle/setup/__init__.py, pyproject.toml's
# `export` group) resolve to the same value; buildscripts/uv_lock_check.py
# enforces the pyproject.toml copy stays in sync.
uv_version="$(cat "$(dirname "${BASH_SOURCE[0]}")/uv_version.txt")"

allow_no_venv=0
python_optarg=""
dry_run=0

while getopts p:fin opt; do
    case "${opt}" in
    p)
        python_optarg="${OPTARG}"
        ;;
    f)
        allow_no_venv=1
        ;;
    n)
        dry_run=1
        ;;
    ?)
        echo "invalid option: ${opt}" >&2
        exit 2
        ;;
    esac
done

run() {
    echo "$@"
    if [[ "${dry_run}" == 1 ]]; then
        return
    fi
    "$@"
}

if [[ "${allow_no_venv}" != 1 && -z "${VIRTUAL_ENV}" ]]; then
    cat <<EOF >&2
Refusing to run without a python virtual env.
See https://github.com/10gen/mongo/blob/master/docs/building.md#python-prerequisites.
Provide the '-f' option to run anyway.
EOF
    exit 1
fi

# Make a structured and user-evident choice of python interpreter.
# Windows venvs use `<venv>/Scripts/python.exe` — Unix venvs use
# `<venv>/bin/python3`. On Cygwin/Git-Bash the `$OS` env var is set to
# `Windows_NT`; that's the most reliable disambiguator (checking for
# `bin/` vs `Scripts/` directly is unreliable because Cygwin can create
# either layout on shared drives).
if [[ "${OS-}" == "Windows_NT" ]]; then
    _venv_python_relpath="Scripts/python.exe"
else
    _venv_python_relpath="bin/python3"
fi

if [[ -n "${python_optarg}" ]]; then
    py3="${python_optarg}"
    echo "Using '${py3}' from explicit \`-p\` option" >&2
elif [[ -n "${VIRTUAL_ENV}" ]]; then
    py3="${VIRTUAL_ENV}/${_venv_python_relpath}"
    echo "Using '${py3}' based on VIRTUAL_ENV=${VIRTUAL_ENV}" >&2
elif [[ -n "${PYTHON3}" ]]; then
    py3="${PYTHON3}"
    echo "Using '${py3}' from PYTHON3 variable" >&2
else
    py3="python3"
    echo "Using '${py3}' as hardcoded fallback value" >&2
fi

# uv ignores $VIRTUAL_ENV for project commands like `uv sync` unless
# explicitly told otherwise — a bare `uv sync` would create and populate
# `<repo>/.venv` instead of the environment `py3` lives in. Point uv at the
# environment we actually intend to sync via UV_PROJECT_ENVIRONMENT
# (same mechanism evergreen/functions/venv_setup.sh uses).
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${python_optarg}" ]]; then
    # Sync the environment the explicitly-chosen interpreter lives in.
    # Refuse non-virtualenv interpreters: `uv sync` uninstalls anything not
    # in uv.lock, which must never happen to a system Python.
    env_prefix="$("${py3}" -c 'import sys; print(sys.prefix)')"
    base_prefix="$("${py3}" -c 'import sys; print(sys.base_prefix)')"
    if [[ "${env_prefix}" == "${base_prefix}" ]]; then
        echo "Refusing to 'uv sync' into non-virtualenv interpreter '${py3}'." >&2
        echo "Activate a virtual env (or pass '-p <venv python>') and re-run." >&2
        exit 1
    fi
    export UV_PROJECT_ENVIRONMENT="${env_prefix}"
elif [[ -n "${VIRTUAL_ENV}" ]]; then
    export UV_PROJECT_ENVIRONMENT="${VIRTUAL_ENV}"
else
    # No venv anywhere (only reachable under '-f'): bootstrap the standard
    # workstation venv at the repo root rather than uv's default `.venv`.
    # This is the contract bazel/wrapper_hook/install_modules.py relies on.
    # Create the venv up front and repoint py3 into it so the pip install
    # of uv below also lands in the venv — never in the system interpreter.
    venv_dir="${repo_root}/python3-venv"
    if [[ ! -e "${venv_dir}/${_venv_python_relpath}" ]]; then
        run "${py3}" -m venv "${venv_dir}"
    fi
    py3="${venv_dir}/${_venv_python_relpath}"
    echo "Using '${py3}' from bootstrapped ${venv_dir}" >&2
    export UV_PROJECT_ENVIRONMENT="${venv_dir}"
fi

# uv is a native Windows binary; under Cygwin/Git-Bash translate the
# environment path to native Windows form so uv can resolve it.
if [[ "${OS-}" == "Windows_NT" ]] && command -v cygpath >/dev/null 2>&1; then
    UV_PROJECT_ENVIRONMENT="$(cygpath -w "${UV_PROJECT_ENVIRONMENT}")"
fi
echo "Syncing environment: ${UV_PROJECT_ENVIRONMENT}" >&2

pip_opts=()
if [[ "${allow_no_venv}" != 1 ]]; then
    # Exploit pip's own enforcement of virtualenv.
    pip_opts+=(--require-virtualenv)
fi

# Install (or upgrade) uv inside the active venv if it isn't already at the
# pinned version. We deliberately install via pip rather than the standalone
# installer so the binary lives next to the interpreter that runs it.
need_uv_install=0
if ! installed_version="$("${py3}" -m pip show uv 2>/dev/null | awk '/^Version:/{print $2}')"; then
    echo "uv not found in this interpreter, installing via pip." >&2
    need_uv_install=1
elif [[ "${installed_version}" != "${uv_version}" ]]; then
    echo "uv ${installed_version} found, expected ${uv_version}; reinstalling." >&2
    need_uv_install=1
fi

if ((need_uv_install)); then
    run "${py3}" -m pip install "${pip_opts[@]}" "uv==${uv_version}"
fi

# `--all-groups` installs every PEP 735 dependency group declared in
# pyproject.toml (mirrors the prior `poetry install --no-root --sync` which
# installed all groups by default). `--no-install-project` is the equivalent
# of `--no-root` — this repo is not itself a package. `--locked` makes a
# stale uv.lock a hard error instead of silently re-resolving and rewriting
# it — uv.lock is consumed directly by Bazel (rules_pycross), so an install-
# time rewrite would silently diverge the venv from the @pypi hub. Fix a
# stale lock deliberately with `uv lock` and commit the result.
run "${py3}" -m uv sync --locked --all-groups --no-install-project
