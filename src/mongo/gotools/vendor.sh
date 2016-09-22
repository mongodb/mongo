#!/usr/bin/env bash

set -eu
set -x
set -o errexit

rm -rf vendor
SCRIPT_DIR="$(cd "$(dirname ${BASH_SOURCE[0]})" && pwd)"
cd $SCRIPT_DIR

set_dependencies() {
  local pids=()
  while read line; do
    local line=`echo $line | sed 's/#.*//;/^\s*$/d' || echo ""`
    [ ! "$line" ] && continue
    line=($line)
    local dest=${line[0]}
    local version=${line[1]}
    if [[ -n ${line[2]:-} ]]; then
      package=${line[2]}
    else
      package=$dest
    fi

    local giturl="https://$package"
    local install_path="vendor/src/$dest"

    mkdir -p "$install_path"

    git clone $giturl "$install_path"

    ( cd $install_path && git checkout  "$version" )
  done < $1

  echo ">> All Done"
}

set_dependencies "Godeps"
