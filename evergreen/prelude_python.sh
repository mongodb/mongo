if [ "Windows_NT" = "$OS" ]; then
    . "$(dirname "${BASH_SOURCE[0]}")/prelude_python_windows.sh" || return 1
    python=$(_windows_python '/cygdrive/c/python/python313/python.exe') || return 1
    echo "Executing on windows, setting python to ${python}"
elif [ "$(uname)" = "Darwin" ]; then
    python='/Library/Frameworks/Python.Framework/Versions/3.13/bin/python3'
    echo "Executing on mac, setting python to ${python}"
else
    # Check if Python 3.10 is requested via Evergreen parameter
    # TODO SERVER-119677: Remove use_python310 parameter once Amazon2 sqlite is upgraded
    if [ "${use_python310}" = "true" ]; then
        if [ -f /opt/mongodbtoolchain/v4/bin/python3 ]; then
            python="/opt/mongodbtoolchain/v4/bin/python3"
            echo "Using Python 3.10 from v4 toolchain (use_python310=true): ${python}"
        else
            echo "ERROR: use_python310 is set but Python 3 is not available at /opt/mongodbtoolchain/v4/bin/python3"
            return 1
        fi
    # Check if v5 toolchain exists - it requires Python 3.13
    elif [ -d /opt/mongodbtoolchain/v5 ]; then
        if [ -f /opt/mongodbtoolchain/v5/bin/python3.13 ]; then
            python="/opt/mongodbtoolchain/v5/bin/python3.13"
            echo "Found python 3.13 in v5 toolchain, setting python to ${python}"
        else
            echo "ERROR: v5 toolchain exists but Python 3.13 is not available at /opt/mongodbtoolchain/v5/bin/python3.13"
            echo "The v5 toolchain requires Python 3.13. Please ensure python3.13 is installed in the toolchain."
            return 1
        fi
    elif [ -f /opt/mongodbtoolchain/v4/bin/python3 ]; then
        python="/opt/mongodbtoolchain/v4/bin/python3"
        echo "Found python in v4 toolchain, setting python to ${python}"
    elif [ -f "$(which python3)" ]; then
        python=$(which python3)
        echo "Could not find mongodbtoolchain python, using system python ${python}"
    else
        echo "Could not find python3."
        return 1
    fi
fi
