if [[ "$0" == *"/evergreen/prelude.sh" ]]; then
  echo "ERROR: do not execute this script. source it instead. i.e.: . prelude.sh"
  exit 1
fi
set -o errexit

# path the directory that contains this script.
evergreen_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"

. "$evergreen_dir/prelude_workdir.sh"
. "$evergreen_dir/prelude_python.sh"
. "$evergreen_dir/prelude_venv.sh"

expansions_yaml="$evergreen_dir/../../expansions.yml"
expansions_default_yaml="$evergreen_dir/../etc/expansions.default.yml"
script="$evergreen_dir/../buildscripts/evergreen_expansions2bash.py"
if [ "Windows_NT" = "$OS" ]; then
  expansions_yaml=$(cygpath -w "$expansions_yaml")
  expansions_default_yaml=$(cygpath -w "$expansions_default_yaml")
  script=$(cygpath -w "$script")
fi

eval $(activate_venv > /dev/null && $python "$script" "$expansions_yaml" "$expansions_default_yaml")
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
  set -o > /tmp/settings.log
  set +o errexit
  grep errexit /tmp/settings.log | grep on
  errexit_on=$?
  # Set errexit "off".
  set +o errexit
  sudo=
  # Use sudo, if it is supported.
  sudo date > /dev/null 2>&1
  if [ $? -eq 0 ]; then
    sudo=sudo
  fi
  # Set errexit "on", if previously enabled.
  if [ $errexit_on -eq 0 ]; then
    set -o errexit
  fi
}
set +o errexit
