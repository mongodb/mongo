#!/usr/bin/env bash

set -eu
# Make sure we're in the directory where the script lives
SCRIPT_DIR="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"
cd $SCRIPT_DIR
echo "Installing dependencies..."

# Set the $GOPATH appropriately so that the dependencies are 
# installed into the vendor directory
export GOPATH=`pwd`/vendor

## Functions/
usage() {
cat << EOF
USAGE
      $ vendor.sh             # Same as 'install'.
      $ vendor.sh install     # Parses the Godeps file, installs dependencies and sets
                              # them to the appropriate version.
      $ vendor.sh version     # Outputs the version of gpm used
      $ vendor.sh help        # Prints this message
EOF
}

# Iterates over Godep file dependencies and sets
# the specified version on each of them.
set_dependencies() {
  local pids=()
  while read line; do
    local line=`echo $line | sed 's/#.*//;/^\s*$/d' || echo ""`
    [ ! "$line" ] && continue
    (
      line=($line)
      local package=${line[0]}
      local version=${line[1]}
      local dest=""
      if [[ -n ${line[2]:-} ]]; then
        dest=$package
        package=${line[2]}
      fi

      if [[ "$OSTYPE" == "cygwin" || "$OSTYPE" == "msys" ]]
      then
        local install_path="${GOPATH%%;*}/src/${package%%/...}"
      else
        local install_path="${GOPATH%%:*}/src/${package%%/...}"
      fi

      [[ -e "$install_path/.git/index.lock" ||
         -e "$install_path/.hg/store/lock"  ||
         -e "$install_path/.bzr/checkout/lock" ]] && wait

      echo ">> Getting package "$package""
      go get -u -d "$package"

      cd $install_path
      hg update     "$version" > /dev/null 2>&1 || \
      git checkout  "$version" > /dev/null 2>&1 || \
      bzr revert -r "$version" > /dev/null 2>&1 || \
      #svn has exit status of 0 when there is no .svn
      { [ -d .svn ] && svn update -r "$version" > /dev/null 2>&1; } || \
      { echo ">> Failed to set $package to version $version"; exit 1; }

      echo ">> Set $package to version $version"
      if [[ -n "$dest" ]] ; then
        if [[ "$OSTYPE" == "cygwin" || "$OSTYPE" == "msys" ]]
        then
          local dest_path="${GOPATH%%;*}/src/${dest%%/...}"
        else
          local dest_path="${GOPATH%%:*}/src/${dest%%/...}"
        fi
        mkdir -p "$(dirname "$dest_path")"
        cd "$(dirname "$dest_path")"
        rm -rf $dest_path
        mv $install_path $dest_path
        echo ">> moved $install_path to $dest_path"
      fi
    ) &
    pids=(${pids[@]-} $!)
  done < $1

  for pid in "${pids[@]-}"; do
      wait $pid
      local status=$?
      [ $status -ne 0 ] && exit $status
  done

  echo ">> All Done"
}
## /Functions

## Command Line Parsing
case "${1:-"install"}" in
  "version")
    echo ">> gpm v1.2.1"
    ;;
  "install")
    deps_file="${2:-"Godeps"}"
    [[ -f "$deps_file" ]] || (echo ">> $deps_file file does not exist." && exit 1)
    (go version > /dev/null) ||
      ( echo ">> Go is currently not installed or in your PATH" && exit 1)
    set_dependencies $deps_file
    ;;
  "help")
    usage
    ;;
  *)
    ## Support for Plugins: if command is unknown search for a gpm-command executable.
    if command -v "gpm-$1" > /dev/null
    then
      plugin=$1 &&
      shift     &&
      gpm-$plugin $@ &&
      exit
    else
      usage && exit 1
    fi
    ;;
esac
