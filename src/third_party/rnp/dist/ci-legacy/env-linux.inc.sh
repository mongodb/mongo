#!/usr/bin/env bash
# shellcheck disable=SC1091

: "${CORES:=$(grep -c '^$' /proc/cpuinfo)}"
export CORES
export MAKE=make

DIST="$(get_linux_dist)"
DIST_VERSION_ID="$(sh -c '. /etc/os-release && echo $VERSION_ID')"
DIST_VERSION="${DIST}-${DIST_VERSION_ID}"

export DIST
export DIST_VERSION
export DIST_VERSION_ID

case "${DIST}" in
  centos|fedora)
    if command -v dnf >/dev/null; then
      export YUM=dnf
    else
      export YUM=yum
    fi
    export SUDO=sudo
    ;;
  ubuntu)
    export SUDO=sudo
    ;;
esac

# XXX: debug function for locale
case "${DIST}" in
  fedora|centos)
    debuglocale() {
      locale -a
      localedef --list-archive
      if ! command -v diff >/dev/null; then
        "${YUM}" -y -q install diffutils
      fi
      bash -c 'diff -u <(localedef --list-archive | sort) <(locale -a | sort) || :'
      localedef -c -i "${LC_ALL%.*}" -f UTF-8 "${LC_ALL}"
      # Error:  character map file `UTF-8' not found: No such file or directory
      # Error:  cannot read character map directory `/usr/share/i18n/charmaps': No such file or directory
      locale -a | grep "${LC_ALL}" || :
    }
    ;;
esac
