#!/bin/bash

set -o errexit

echo "+-----------------------------------------------------------------------------------------------------------+"
echo "|   Running a script to automatically install db-contrib-tool (https://github.com/10gen/db-contrib-tool).   |"
echo "+-----------------------------------------------------------------------------------------------------------+"
echo

if [[ -d "/opt/mongodbtoolchain/v4/bin" ]]; then
    export PATH="/opt/mongodbtoolchain/v4/bin:$PATH"
fi

if [[ -d "/opt/mongodbtoolchain/v5/bin" ]]; then
    export PATH="/opt/mongodbtoolchain/v5/bin:$PATH"
fi

rc_file=""
if [[ -f "$HOME/.bashrc" ]]; then
    rc_file="$HOME/.bashrc"
fi

if [[ -f "$HOME/.zshrc" ]]; then
    rc_file="$HOME/.zshrc"
fi

if ! command -v db-contrib-tool &>/dev/null; then
    if ! python3 -c "import sys; sys.exit(sys.version_info < (3, 7))" &>/dev/null; then
        actual_version=$(python3 -c 'import sys; print(sys.version)')
        echo "You must have python3.7+ installed. Detected version $actual_version."
        echo "To avoid unexpected issues, python3.7+ will not be automatically installed."
        echo "Please, do it yourself."
        echo
        echo "On macOS you can run:"
        echo
        echo "    brew install python3"
        echo
        exit 1
    fi

    if command -v pipx &>/dev/null; then
        echo "Found pipx: $(command -v pipx)."
        echo "Using it to install 'db-contrib-tool'."
        echo

        pipx ensurepath &>/dev/null
        if [[ -f "$rc_file" ]]; then
            source "$rc_file"
        fi

        pipx install db-contrib-tool --python $(command -v python3) --force
        echo
    else
        if ! python3 -m pipx --version &>/dev/null; then
            echo "Couldn't find pipx. Installing it as python3 module:"
            echo "    $(command -v python3) -m pip install pipx"
            echo
            python3 -m pip install pipx
            echo
        else
            echo "Found pipx installed as python3 module:"
            echo "    $(command -v python3) -m pipx --version"
            echo "Using it to install 'db-contrib-tool'."
            echo
        fi

        python3 -m pipx ensurepath &>/dev/null
        if [[ -f "$rc_file" ]]; then
            source "$rc_file"
        fi

        python3 -m pipx install db-contrib-tool --force
        echo
    fi
fi

echo "Please, open a new shell or run:"
echo
echo "    source $rc_file"
echo
