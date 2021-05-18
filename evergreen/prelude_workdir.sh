calculated_workdir=$(cd "$evergreen_dir/../.." && echo "$PWD")
pwd_cygpath="$PWD"
if [ "Windows_NT" = "$OS" ]; then
  calculated_workdir=$(cygpath -w "$calculated_workdir")
  pwd_cygpath=$(cygpath -w "$pwd_cygpath")
fi
if [ -z "$workdir" ]; then
  workdir="$calculated_workdir"

# skip this test on Windows. The directories will never match due to the many
# different path types present on Windows+Cygwin
elif [ "$workdir" != "$calculated_workdir" ] && [ "Windows_NT" != "$OS" ]; then
  # if you move the checkout directory (ex: simple project config project),
  # then this assertion will fail in the future. You need to update
  # calculated_workdir, and all the relative directories in this file.
  echo "\$workdir was specified, but didn't match \$calculated_workdir. Did the directory structure change? Update prelude.sh"
  echo "\$workdir: $workdir"
  echo "\$calculated_workdir: $calculated_workdir"
  exit 1
fi
if [ "$pwd_cygpath" != "$calculated_workdir" ]; then
  echo "ERROR: Your script changed directory before loading prelude.sh. Don't do that"
  echo "\$PWD: $PWD"
  echo "\$calculated_workdir: $calculated_workdir"
  exit 1
fi
