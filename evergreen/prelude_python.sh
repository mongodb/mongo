if [ "Windows_NT" = "$OS" ]; then
    python='/cygdrive/c/python/python310/python.exe'
    echo "Executing on windows, setting python to ${python}"
elif [ "$(uname)" = "Darwin" ]; then
    python='/Library/Frameworks/Python.Framework/Versions/3.10/bin/python3'
    echo "Executing on mac, setting python to ${python}"
else
    if [ -f /opt/mongodbtoolchain/v5/bin/python3 ]; then
        python="/opt/mongodbtoolchain/v5/bin/python3"
        echo "Found python in v5 toolchain, setting python to ${python}"
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
