if [[ "$0" == *"/evergreen/prelude.sh" ]]; then
  echo "ERROR: do not execute this script. source it instead. ie: . prelude.sh"
  exit 1
fi

# path the directory that contains this script.
evergreen_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"

# bootstrapping python assumes that the user has not cd'd before the prelude.
# Ensure that here.
calculated_workdir=$(cd "$evergreen_dir/../.." && echo $PWD)
if [ "$PWD" != "$calculated_workdir" ]; then
  echo "ERROR: Your script changed directory before loading prelude.sh. Don't do that"
  echo "\$PWD: $PWD"
  echo "\$calculated_workdir: $calculated_workdir"
  exit 1
fi

function activate_venv {
  set -e
  # check if virtualenv is set up
  if [ -d "${workdir}/venv" ]; then
    if [ "Windows_NT" = "$OS" ]; then
      # Need to quote the path on Windows to preserve the separator.
      . "${workdir}/venv/Scripts/activate" 2>/tmp/activate_error.log
    else
      . ${workdir}/venv/bin/activate 2>/tmp/activate_error.log
    fi
    if [ $? -ne 0 ]; then
      echo "Failed to activate virtualenv: $(cat /tmp/activate_error.log)"
    fi
    python=python
  else
    python=${python:-/opt/mongodbtoolchain/v3/bin/python3}
  fi

  if [ "Windows_NT" = "$OS" ]; then
    export PYTHONPATH="$PYTHONPATH;$(cygpath -w ${workdir}/src)"
  else
    export PYTHONPATH="$PYTHONPATH:${workdir}/src"
  fi

  echo "python set to $(which $python)"
  set +e
}

expansions_yaml="$evergreen_dir/../../expansions.yml"
expansions_default_yaml="$evergreen_dir/../etc/expansions.default.yml"
script="$evergreen_dir/../buildscripts/evergreen_expansions2bash.py"
if [ "Windows_NT" = "$OS" ]; then
  expansions_yaml=$(cygpath -w "$expansions_yaml")
  expansions_default_yaml=$(cygpath -w "$expansions_default_yaml")
  script=$(cygpath -w "$script")
fi

eval $(activate_venv >/dev/null && $python "$script" "$expansions_yaml" "$expansions_default_yaml")
if [ -n "$___expansions_error" ]; then
  echo $___expansions_error
  exit 1
fi
unset expansions_yaml
unset expansions_default_yaml
unset script
unset evergreen_dir

function add_nodejs_to_path {
  # Add node and npm binaries to PATH
  if [ "Windows_NT" = "$OS" ]; then
    # An "npm" directory might not have been created in %APPDATA% by the Windows installer.
    # Work around the issue by specifying a different %APPDATA% path.
    # See: https://github.com/nodejs/node-v0.x-archive/issues/8141
    export APPDATA=${workdir}/npm-app-data
    export PATH="$PATH:/cygdrive/c/Program Files (x86)/nodejs" # Windows location
    # TODO: this is to work around BUILD-8652
    cd "$(pwd -P | sed 's,cygdrive/c/,cygdrive/z/,')"
  else
    export PATH="$PATH:/opt/node/bin"
  fi
}

function posix_workdir {
  if [ "Windows_NT" = "$OS" ]; then
    echo $(cygpath -u "${workdir}")
  else
    echo ${workdir}
  fi
}

function set_sudo {
  set -o >/tmp/settings.log
  set +o errexit
  grep errexit /tmp/settings.log | grep on
  errexit_on=$?
  # Set errexit "off".
  set +o errexit
  sudo=
  # Use sudo, if it is supported.
  sudo date >/dev/null 2>&1
  if [ $? -eq 0 ]; then
    sudo=sudo
  fi
  # Set errexit "on", if previously enabled.
  if [ $errexit_on -eq 0 ]; then
    set -o errexit
  fi
}
