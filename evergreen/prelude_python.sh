if [ "Windows_NT" = "$OS" ]; then
  python='/cygdrive/c/python/python37/python.exe'
else
  if [ -f /opt/mongodbtoolchain/v3/bin/python3 ]; then
    python="/opt/mongodbtoolchain/v3/bin/python3"
  elif [ -f "$(which python3)" ]; then
    echo "Could not find mongodbtoolchain python, using system python $(which python3)" > 2
    python=$(which python3)
  else
    echo "Could not find python3." > 2
    return 1
  fi
fi
