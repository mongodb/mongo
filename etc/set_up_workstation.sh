#! /bin/bash

silent_grep() {
    command grep -q "$@" >/dev/null 2>&1
}

idem_file_append() {
    if [[ -z "$1" ]]; then
        return 1
    fi
    if [[ ! -f "$1" && -n "${4-}" ]]; then
        return
    fi
    if [[ -z "$2" ]]; then
        return 2
    fi
    if [[ -z "$3" ]]; then
        return 3
    fi
    local start_marker="# BEGIN $2"
    local end_marker="# END $2"
    if ! silent_grep "^$start_marker" "$1"; then
        {
            echo -e "\n$start_marker"
            echo -e "$3"
            echo -e "$end_marker"
        } >>"$1"
    fi
}

setup_bash() {
    # Bash profile should source .bashrc
    echo "################################################################################"
    echo "Setting up bash..."
    local block=$(
        cat <<BLOCK
if [[ -f ~/.bashrc ]]; then
    source ~/.bashrc
fi
BLOCK
    )

    idem_file_append ~/.bash_profile "Source .bashrc" "$block"

    set +o nounset
    source ~/.bash_profile
    set -o nounset

    echo "Finished setting up ~/.bashprofile..."
}

setup_mongo_venv() {
    echo "################################################################################"
    echo "Setting up the local virtual environment..."

    # PYTHON_KEYRING_BACKEND is needed to make poetry install work
    # See guide https://wiki.corp.mongodb.com/display/KERNEL/Virtual+Workstation
    export PYTHON_KEYRING_BACKEND=keyring.backends.null.Keyring
    /opt/mongodbtoolchain/v4/bin/python3 -m venv python3-venv

    source ./python3-venv/bin/activate
    POETRY_VIRTUALENVS_IN_PROJECT=true poetry install --no-root --sync
    deactivate

    echo "Finished setting up the local virtual environment..."
    echo "Activate it by running 'source python3-venv/bin/activate'"
}

setup_poetry() {
    echo "################################################################################"
    echo "Installing 'poetry' command..."
    export PATH="$PATH:$HOME/.local/bin"
    if command -v poetry &>/dev/null; then
        echo "'poetry' command exists; skipping setup"
    else
        pipx install poetry --pip-args="-r $(pwd)/poetry_requirements.txt"
        echo "Finished installing poetry..."
    fi
}

setup_pipx() {
    echo "################################################################################"
    echo "Installing 'pipx' command..."
    if command -v pipx &>/dev/null; then
        echo "'pipx' command exists; skipping setup"
    else
        export PATH="$PATH:$HOME/.local/bin"
        local venv_name="tmp-pipx-venv"
        /opt/mongodbtoolchain/v4/bin/python3 -m venv $venv_name

        # virtualenv doesn't like nounset
        set +o nounset
        source $venv_name/bin/activate
        set -o nounset

        python -m pip install --upgrade "pip<20.3"
        python -m pip install pipx

        pipx install pipx --python /opt/mongodbtoolchain/v4/bin/python3 --force
        pipx ensurepath --force

        set +o nounset
        deactivate
        set -o nounset

        rm -rf $venv_name

        source ~/.bashrc

        echo "Finished installing pipx..."
    fi
}

setup_db_contrib_tool() {
    echo "################################################################################"
    echo "Installing 'db-contrib-tool' command..."
    export PATH="$PATH:$HOME/.local/bin"
    if command -v db-contrib-tool &>/dev/null; then
        echo "'db-contrib-tool' command exists; skipping setup"
    else
        pipx install db-contrib-tool
        echo "Finished installing db-contrib-tool"
    fi
}

setup_clang_config() {
    echo "################################################################################"
    echo "Installing clang config..."

    MONGO_WRAPPER_OUTPUT_ALL=1 bazel build compiledb

    echo "Finished installing clang config..."
}

setup_gdb() {
    echo "################################################################################"
    echo "Setting up GDB..."

    cwd=$(pwd)
    cd ..
    if [[ -d 'Boost-Pretty-Printer' ]]; then
        echo "'Boost-Pretty-Printer' dir exists; skipping setup"
    else
        git clone https://github.com/mongodb-forks/Boost-Pretty-Printer.git

        # the original version of this script just appended this line, so we
        # have to grep for it manually
        if ! silent_grep "source $HOME/gdbinit" ~/.gdbinit; then
            idem_file_append ~/.gdbinit "Server Workflow Tool gdbinit" "source $HOME/gdbinit"
        fi

        echo "Finished installing pretty printers..."
    fi
    cd $cwd
}

run_setup() {
    set +o nounset
    source ~/.bashrc
    set -o nounset

    setup_bash

    setup_clang_config
    setup_gdb
    setup_pipx
    setup_db_contrib_tool # This step requires `setup_pipx` to have been run.
    setup_poetry          # This step requires `setup_pipx` to have been run.

    setup_mongo_venv # This step requires `setup_poetry` to have been run.

    echo "Please run 'source ~/.bashrc' to complete setup!"
}

run_setup
