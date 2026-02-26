#!/usr/bin/env bash
# Derived from: https://gist.github.com/marcusandre/4b88c2428220ea255b83
get_os() {
  local ostype
  ostype=$(<<< "$OSTYPE" tr '[:upper:]' '[:lower:]')
  if [ -z "$ostype" ]; then
    ostype=$(uname | tr '[:upper:]' '[:lower:]')
  fi

  case $ostype in
    freebsd*) echo "freebsd" ;;
    netbsd*) echo "netbsd" ;;
    openbsd*) echo "openbsd" ;;
    darwin*) echo "macos" ;;
    linux*) echo "linux" ;;
    cygwin*) echo "cygwin" ;;
    msys*) echo "msys" ;;
    mingw*) echo "win" ;;
    *) echo "unknown"; exit 1 ;;
  esac
}

get_linux_dist() {
  if [[ -f /etc/os-release ]]; then
    sh -c '. /etc/os-release && echo $ID'
  elif type lsb_release >/dev/null 2>&1; then
    lsb_release -si | tr '[:upper:]' '[:lower:]'
  fi
}

# If target does not exist, create symlink from source to target.
ensure_symlink_to_target() {
  local from="${1:?Missing source}"
  local to="${2:?Missing target}"

  if [[ -e "${from}" && ! -e "${to}" ]]; then
    if ! sudo ln -s "${from}" "${to}"
    then
      >&2 echo "Error: ${to} still not available after symlink.  Aborting."
      exit 1
    fi
  fi
}
