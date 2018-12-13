#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"
DEFAULT_WORKING_DIR="${SCRIPT_DIR}/../build/pip"

showUsage() {
    cat <<EOF
USAGE:
    generate-pip-constraints.sh [-o CON_FILE] ...
    generate-pip-constraints.sh -h

    -h, --help      Show this message
    -o CON_FILE     Write constraints.txt to CON_FILE

This command passes all unrecognized arguments to two pip invocations,
one for a python2 virtual environment and one for a python3 virtual environment.
It then forms a unified multi-version constraints file at constraints.txt in its working directory.

This script's working directory currently defaults to '${DEFAULT_WORKING_DIR}'.
This default can be overriden via \$WORKING_DIR.
EOF
}

CON_FILE=""
ARGS=()
while [[ $# -gt 0 ]]
do KEY="${1}"
    case "$KEY" in
        (-h|--help) showUsage; exit 0;;
        (-o) CON_FILE="${2}"; shift 2;;
        (*) ARGS+=("${KEY}"); shift;;
    esac
done

if [[ ${#ARGS[@]} -eq 0 ]]; then
    1>&2 echo "No pip arguments given. Failing..."
    exit 2
fi

generateConstraints(){
    EXE="${1}"
    DIR="${2}"
    if ! (
        export VIRTUAL_ENV_DISABLE_PROMPT=yes
        virtualenv --python "${EXE}" "${DIR}"
        . "${DIR}"/*/activate
        pip install "${ARGS[@]}"
        pip freeze >"${DIR}/requirements.txt"
    )
    then RC=$?
        1>&2 echo "Errors occured while attempting
          to install all requirements into '${DIR}'
          with python executable '${EXE}'"
        return $RC
    fi
}

WORKING_DIR="${WORKING_DIR:-${DEFAULT_WORKING_DIR}}"
if [[ -d $WORKING_DIR ]]; then
    1>&2 echo "Removing existing working dir at '$WORKING_DIR'..."
    rm -r "${WORKING_DIR}"
fi
ABSOLUTE_WORKING_DIR="$(mkdir -p "${WORKING_DIR}" && cd "${WORKING_DIR}" && pwd)"

PIP2_DIR="${ABSOLUTE_WORKING_DIR}/python2"
PIP3_DIR="${ABSOLUTE_WORKING_DIR}/python3"

generateConstraints python2 "${PIP2_DIR}"
generateConstraints python3 "${PIP3_DIR}"

if [[ -z $CON_FILE ]]; then
    CON_FILE="${ABSOLUTE_WORKING_DIR}/constraints.txt"
fi
(
    printf '# == PLEASE DO NOT MANUALLY EDIT THIS FILE =='
    printf '\n# For more details, see etc/pip/README.md'
    printf '\n#'
    printf '\n# This file was generated via the following command:'
    printf "\n# $ 'bash' 'buildscripts/generate-pip-constraints.sh'"
    printf " '%s'" "${ARGS[@]}"
    printf '\n'

    printf '\n# Common requirements\n'
    comm -12 "${PIP2_DIR}/requirements.txt" "${PIP3_DIR}/requirements.txt"

    printf '\n# Python2 requirements\n'
    comm -23 "${PIP2_DIR}/requirements.txt" "${PIP3_DIR}/requirements.txt" |
        sed -e 's/$/; python_version < "3"/'

    printf '\n# Python3 requirements\n'
    comm -13 "${PIP2_DIR}/requirements.txt" "${PIP3_DIR}/requirements.txt" |
        sed -e 's/$/; python_version > "3"/'

    printf '\n'
    cat "${SCRIPT_DIR}/../etc/pip/components/platform.req"
) >"${CON_FILE}"

1>&2 echo "All pip requirements were successfully installed in a virtual environment.
See '${CON_FILE}' for all installed packages."
