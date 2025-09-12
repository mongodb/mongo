#! /bin/bash

failed_setup=false

source ~/.bashrc

if command -v pipx &>/dev/null; then
    echo "'pipx' command exists"
else
    echo "pipx command not found - failed setup"
    failed_setup=true
fi

if command -v poetry &>/dev/null; then
    echo "'poetry' command exists"
else
    echo "poetry command not found - failed setup"
    failed_setup=true
fi

if command -v db-contrib-tool &>/dev/null; then
    echo "'db-contrib-tool' command exists"
else
    echo "db-contrib-tool command not found - failed setup"
    failed_setup=true
fi

if test -d "./python3-venv"; then
    echo "Venv directory exists, checking activation"
    . python3-venv/bin/activate
    ./buildscripts/resmoke.py run --help &>/dev/null
    if [ $? -eq 0 ]; then
        echo "Virtual workstation set up correctly"
    else
        echo "Virtual workstation failed activation"
        failed_setup=true
    fi
    deactivate
else
    echo "mongo virtual environment not created correctly - failed setup"
    failed_setup=true
fi

if test -d "../Boost-Pretty-Printer"; then
    echo "Pretty printers set up correctly"
else
    echo "Pretty printers failed setup"
    failed_setup=true
fi

if test -f "./compile_commands.json"; then
    echo "Clang configuration set up correctly"
else
    echo "Clang configuration failed setup"
    failed_setup=true
fi

if $failed_setup; then
    exit 1
fi
