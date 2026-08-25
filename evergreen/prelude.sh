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
. "$evergreen_dir/prelude_db_contrib_tool.sh"

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

# Custom builds can set the custom_ld_library_path expansion so the produced
# binaries find their consumer-specific libraries at run time. A no-op unless
# the expansion is set; see etc/evergreen_yml_components/custom_builds/README.md.
if [ -n "${custom_ld_library_path:-}" ]; then
  export LD_LIBRARY_PATH="${custom_ld_library_path}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

unset expansions_yaml
unset expansions_default_yaml
unset script
unset evergreen_dir

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
