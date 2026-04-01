if [ "Windows_NT" = "$OS" ]; then
  # `set up venv` runs before Evergreen expansions are loaded into bash, so
  # default Windows bootstrap to Python 3.10 instead of the legacy 3.7 path.
  python="${python:-/cygdrive/c/python/python310/python.exe}"
else
  if [ -f /opt/mongodbtoolchain/v4/bin/python3 ]; then
    python="/opt/mongodbtoolchain/v4/bin/python3"
  elif [ -f "$(which python3)" ]; then
    echo "Could not find mongodbtoolchain python, using system python $(which python3)" > 2
    python=$(which python3)
  else
    echo "Could not find python3." > 2
    return 1
  fi
fi
