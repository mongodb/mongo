#!/bin/bash
# Run this in a mongo git repo to setup the Python dependencies
# needed to build that repo, as specified in `poetry.lock`.
#
# If your virtual env has been activated, you can usually just
# execute this script with no arguments.
#
# Optionally override the Python interpreter with `$PYTHON3`, or
# with the `-p <path_to_python3>` option.  It defaults to
# `python3`, which is usually correct.
#
# Normally, this script refuses to install unless a virtual env is
# active. This avoids making accidental system-wide changes. A user
# can override this safety measure with `-f`.
#
# Our workspace setup requires a specific version of poetry to be
# installed, this script automates the pip install of that version.

poetry_version='1.8.3'

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

run () {
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
if [[ -n "${python_optarg}" ]]; then
    py3="${python_optarg}"
    echo "Using '${py3}' from explicit \`-p\` option" >&2
elif [[ -n "${VIRTUAL_ENV}" ]]; then
    py3="${VIRTUAL_ENV}/bin/python3"
    echo "Using '${py3}' based on VIRTUAL_ENV=${VIRTUAL_ENV}" >&2
elif [[ -n "${PYTHON3}" ]]; then
    py3="${PYTHON3}"
    echo "Using '${py3}' from PYTHON3 variable" >&2
else
    py3="python3"
    echo "Using '${py3}' as hardcoded fallback value" >&2
fi

pip_opts=()
if [[ "${allow_no_venv}" != 1 ]]; then
    # Exploit pip's own enforcement of virtualenv.
    pip_opts+=('--require-virtualenv')
fi
run "${py3}" -m pip install "${pip_opts[@]}" "poetry==${poetry_version}"

run env \
    PYTHON_KEYRING_BACKEND="keyring.backends.null.Keyring" \
    "${py3}" -m poetry install --no-root --sync

# poetry will install cryptography in an isolated build environment
# to conform to pep517, however this doesn't work for the old cryptography
# version on these platforms, and ends up not building required shared libraries.
# Here we go behing poetry's back and install with pip
if uname -a | grep -q 's390x\|ppc64le'; then
    "${py3}" -m pip uninstall -y cryptography==2.3
    "${py3}" -m pip install cryptography==2.3
fi
