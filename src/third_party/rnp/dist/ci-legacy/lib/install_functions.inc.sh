#!/usr/bin/env bash
# shellcheck disable=SC1090
# shellcheck disable=SC1091
# shellcheck disable=SC2164

: "${GPG_VERSION:=stable}"
: "${BUILD_SHARED_LIBS:=off}"
: "${USE_STATIC_DEPENDENCIES:=}"
: "${OS:=}"
: "${DIST:=}"
: "${DIST_VERSION:=}"
: "${DIST_VERSION_ID:=}"

: "${MINIMUM_CMAKE_VERSION:=3.20.0}"
: "${MINIMUM_RUBY_VERSION:=3.0.0}"

: "${RECOMMENDED_BOTAN_VERSION:=2.18.2}"
: "${RECOMMENDED_JSONC_VERSION:=0.12.1}"
: "${RECOMMENDED_CMAKE_VERSION:=3.20.5}"
: "${RECOMMENDED_PYTHON_VERSION:=3.9.2}"
: "${RECOMMENDED_RUBY_VERSION:=3.1.1}"

: "${CMAKE_VERSION:=${RECOMMENDED_CMAKE_VERSION}}"
: "${BOTAN_VERSION:=${RECOMMENDED_BOTAN_VERSION}}"
: "${JSONC_VERSION:=${RECOMMENDED_JSONC_VERSION}}"
: "${PYTHON_VERSION:=${RECOMMENDED_PYTHON_VERSION}}"
: "${RUBY_VERSION:=${RECOMMENDED_RUBY_VERSION}}"

# Should minimum automake version change
# please consider release of Ribose RPM for it
# [https://github.com/riboseinc/rpm-spec-automake116-automake]

: "${MINIMUM_AUTOMAKE_VERSION:=1.16.3}"
: "${RECOMMENDED_AUTOMAKE_VERSION:=1.16.4}"
: "${AUTOMAKE_VERSION:=${RECOMMENDED_AUTOMAKE_VERSION}}"

: "${VERBOSE:=1}"


if [[ "${OS}" = "freebsd" ]] || \
   [[ "${DIST}" = "ubuntu" ]] || \
   [[ "${DIST}" = "centos" ]] || \
   [[ "${DIST}" = "fedora" ]]
then
  SUDO="${SUDO:-sudo}"
else
  SUDO="${SUDO:-run}"
fi

# Simply run its arguments.
run() {
  "$@"
}

. ci/lib/cacheable_install_functions.inc.sh

freebsd_install() {
  local packages=(
    git
    readline
    bash
    gnupg
    devel/pkgconf
    wget
    cmake
    gmake
    autoconf
    automake
    libtool
    gettext-tools
    python
    lang/ruby27
  )

  # Note: we assume sudo is already installed
  "${SUDO}" pkg install -y "${packages[@]}"

  cd /usr/ports/devel/ruby-gems
  "${SUDO}" make -DBATCH RUBY_VER=2.7 install
  cd

  mkdir -p ~/.gnupg
  echo "disable-ipv6" >> ~/.gnupg/dirmngr.conf
  dirmngr </dev/null
  dirmngr --daemon
  ensure_automake
}

openbsd_install() {
  echo ""
}

netbsd_install() {
  echo ""
}

linux_prepare_ribose_yum_repo() {
  "${SUDO}" rpm --import https://github.com/riboseinc/yum/raw/master/ribose-packages.pub
  "${SUDO}" rpm --import https://github.com/riboseinc/yum/raw/master/ribose-packages-next.pub
  "${SUDO}" curl -L https://github.com/riboseinc/yum/raw/master/ribose.repo \
    -o /etc/yum.repos.d/ribose.repo
}

# Prepare the system by updating and optionally installing repos
yum_prepare_repos() {
  yum_install "${util_dependencies_yum[@]}"
  linux_prepare_ribose_yum_repo
  "${SUDO}" "${YUM}" -y update
  if [[ $# -gt 0 ]]; then
    yum_install "$@"
  fi
}

linux_install_fedora() {
  yum_prepare_repos
  extra_dep=(cmake json-c-devel ruby)

  yum_install_build_dependencies "${extra_dep[@]}"
  yum_install_dynamic_build_dependencies_if_needed

  ensure_automake
  ensure_cmake
#  ensure_ruby
  rubygem_install_build_dependencies
}

linux_install_centos() {
  case "${DIST_VERSION}" in
    centos-7)
      linux_install_centos7
      ;;
    centos-8)
      linux_install_centos8
      ;;
    centos-9)
      linux_install_centos9
      ;;
    *)
      >&2 echo "Error: unsupported CentOS version \"${DIST_VERSION_ID}\" (supported: 7, 8, 9).  Aborting."
      exit 1
  esac
}

declare util_dependencies_yum=(
  sudo
  wget
  git
)

declare basic_build_dependencies_yum=(
  # cmake3 # XXX: Fedora 22+ only has cmake
  clang
  gcc
  gcc-c++
  make
  autoconf
  libtool
  bzip2
  gzip
  ribose-automake116
)

declare build_dependencies_yum=(
  bison
  byacc
  bzip2-devel
  gettext-devel
  ncurses-devel
  python3
#  ruby-devel
  zlib-devel
)

declare dynamic_build_dependencies_yum=(
  botan2
  botan2-devel
)

apt_install() {
  local apt_command=(apt-get -y -q install "$@")
  if command -v sudo >/dev/null; then
    sudo "${apt_command[@]}"
  else
    "${apt_command[@]}"
  fi
}

yum_install() {
  local yum_command=("${YUM}" -y -q install "$@")
  if command -v sudo >/dev/null; then
    sudo "${yum_command[@]}"
  else
    "${yum_command[@]}"
  fi
}

prepare_build_tool_env() {
  enable_llvm_toolset_7
  enable_rh_ruby30
  enable_ribose_automake
#  prepare_rbenv_env
}

yum_install_build_dependencies() {
  yum_install \
    "${basic_build_dependencies_yum[@]}" \
    "${build_dependencies_yum[@]}" \
    "$@"

  if [[ "${CRYPTO_BACKEND:-}" == "openssl" ]]; then
    yum_install openssl-devel
  fi
}

linux_install_centos7() {
  yum_prepare_repos epel-release centos-release-scl centos-sclo-rh

  extra_dep=(cmake3 llvm-toolset-7.0 json-c12-devel rh-ruby30)

  yum_install_build_dependencies "${extra_dep[@]}"
  yum_install_dynamic_build_dependencies_if_needed

  ensure_automake
  ensure_cmake
#  ensure_ruby
  enable_rh_ruby30
  rubygem_install_build_dependencies
}

linux_install_centos8() {
  "${SUDO}" "${YUM}" -y -q install 'dnf-command(config-manager)'
  "${SUDO}" "${YUM}" config-manager --set-enabled powertools
  "${SUDO}" "${YUM}" module reset ruby -y
  yum_prepare_repos epel-release

  extra_dep=(cmake texinfo json-c-devel @ruby:3.0)

  yum_install_build_dependencies "${extra_dep[@]}"
  yum_install_dynamic_build_dependencies_if_needed

  ensure_automake
  ensure_cmake
# ensure_ruby
  ensure_symlink_to_target /usr/bin/python3 /usr/bin/python
  rubygem_install_build_dependencies
}

linux_install_centos9() {
  "${SUDO}" "${YUM}" -y -q install 'dnf-command(config-manager)'
  "${SUDO}" "${YUM}" config-manager --set-enabled crb
  yum_prepare_repos epel-release

  extra_dep=(cmake texinfo json-c-devel ruby)

  yum_install_build_dependencies "${extra_dep[@]}"
  yum_install_dynamic_build_dependencies_if_needed

  ensure_automake
  ensure_cmake
#  ensure_ruby
  rubygem_install_build_dependencies
}

is_use_static_dependencies() {
  [[ -n "${USE_STATIC_DEPENDENCIES}" ]] && \
    [[ no    != "${USE_STATIC_DEPENDENCIES}" ]] && \
    [[ off   != "${USE_STATIC_DEPENDENCIES}" ]] && \
    [[ false != "${USE_STATIC_DEPENDENCIES}" ]] && \
    [[ 0     != "${USE_STATIC_DEPENDENCIES}" ]]
}

yum_install_dynamic_build_dependencies_if_needed() {
  if ! is_use_static_dependencies; then
    yum_install_dynamic_build_dependencies
  fi
}

install_static_noncacheable_build_dependencies_if_needed() {
  if is_use_static_dependencies; then
    install_static_noncacheable_build_dependencies "$@"
  fi
}

install_static_cacheable_build_dependencies_if_needed() {
  if is_use_static_dependencies || [[ "$#" -gt 0 ]]; then
    USE_STATIC_DEPENDENCIES=true
    install_static_cacheable_build_dependencies "$@"
  fi
}

install_static_cacheable_build_dependencies() {
  prepare_build_tool_env

  mkdir -p "$LOCAL_BUILDS"

  local default=(jsonc gpg)
  if [[ "${CRYPTO_BACKEND:-}" != "openssl" ]]; then
    default=(botan "${default[@]}")
  fi
  local items=("${@:-${default[@]}}")
  for item in "${items[@]}"; do
    install_"$item"
  done
}

install_static_noncacheable_build_dependencies() {
  mkdir -p "$LOCAL_BUILDS"

  local default=(asciidoctor)
  local items=("${@:-${default[@]}}")
  for item in "${items[@]}"; do
    install_"$item"
  done
}

rubygem_install_build_dependencies() {
  install_asciidoctor
}

yum_install_dynamic_build_dependencies() {
  yum_install \
    "${dynamic_build_dependencies_yum[@]}"

  # Work around pkg-config giving out wrong include path for json-c:
  ensure_symlink_to_target /usr/include/json-c12 /usr/include/json-c
}

# Make sure cmake is at least 3.14+ as required by rnp
# Also make sure ctest is available.
# If not, build cmake from source.
ensure_cmake() {
  ensure_symlink_to_target /usr/bin/cmake3 /usr/bin/cmake
  ensure_symlink_to_target /usr/bin/cpack3 /usr/bin/cpack

  local cmake_version
  cmake_version=$({
    command -v cmake  >/dev/null && command cmake --version || \
    command -v cmake3 >/dev/null && command cmake3 --version
    } | head -n1 | cut -f3 -d' '
  )

  local need_to_build_cmake=

  # Make sure ctest is also in PATH.  If not, build cmake from source.
  # TODO: Check CentOS7 tests in GHA.
  if ! command -v ctest >/dev/null; then
    >&2 echo "ctest not found."
    need_to_build_cmake=1
  elif ! is_version_at_least cmake "${MINIMUM_CMAKE_VERSION}" echo "${cmake_version}"; then
    >&2 echo "cmake version lower than ${MINIMUM_CMAKE_VERSION}."
    need_to_build_cmake=1
  fi

  if [[ "${need_to_build_cmake}" != 1 ]]; then
    CMAKE="$(command -v cmake)"
    >&2 echo "cmake rebuild is NOT needed."
    return
  fi

  >&2 echo "cmake rebuild is needed."

  pushd "$(mktemp -d)" || return 1

  install_prebuilt_cmake Linux-x86_64
#  build_and_install_cmake

  command -v cmake

  popd

  # Abort if ctest still not found.
  if ! command -v ctest >/dev/null; then
    >&2 echo "Error: ctest not found.  Aborting."
    exit 1
  fi
}

# E.g. for i386
# NOTE: Make sure cmake's build prerequisites are installed.
build_and_install_cmake() {
  local cmake_build=${LOCAL_BUILDS}/cmake
  mkdir -p "${cmake_build}"
  pushd "${cmake_build}"
  wget https://github.com/Kitware/CMake/releases/download/v"${CMAKE_VERSION}"/cmake-"${CMAKE_VERSION}".tar.gz -O cmake.tar.gz
  tar xzf cmake.tar.gz --strip 1

  PREFIX="${PREFIX:-/usr}"
  mkdir -p "${PREFIX}"
  ./configure --prefix="${PREFIX}" && ${MAKE} -j"${MAKE_PARALLEL}" && "${SUDO}" make install
  popd
  CMAKE="${PREFIX}"/bin/cmake
}

# 'arch' corresponds to the last segment of GitHub release URL
install_prebuilt_cmake() {
  local arch="${1:?Missing architecture}"
  local cmake_build=${LOCAL_BUILDS}/cmake
  mkdir -p "${cmake_build}"
  pushd "${cmake_build}"
  curl -L -o \
    cmake.sh \
    https://github.com/Kitware/CMake/releases/download/v"${CMAKE_VERSION}"/cmake-"${CMAKE_VERSION}"-"${arch}".sh

  PREFIX="${PREFIX:-/usr}"
  mkdir -p "${PREFIX}"
  "${SUDO}" sh cmake.sh --skip-license --prefix="${PREFIX}"
  popd
  CMAKE="${PREFIX}"/bin/cmake
}

build_and_install_python() {
  python_build=${LOCAL_BUILDS}/python
  mkdir -p "${python_build}"
  pushd "${python_build}"
  curl -L -o python.tar.xz https://www.python.org/ftp/python/"${PYTHON_VERSION}"/Python-"${PYTHON_VERSION}".tar.xz
  tar -xf python.tar.xz --strip 1
  ./configure --enable-optimizations --prefix=/usr && ${MAKE} -j"${MAKE_PARALLEL}" && "${SUDO}" make install
  ensure_symlink_to_target /usr/bin/python3 /usr/bin/python
  popd
}

# Make sure automake is at least $MINIMUM_AUTOMAKE_VERSION (1.16.3) as required by GnuPG 2.3
# - We assume that on fedora/centos ribose rpm was used (see basic_build_dependencies_yum)
# - If automake version is less then required automake build it from source
ensure_automake() {

  local using_ribose_automake=
  enable_ribose_automake

  local automake_version=
  automake_version=$({
    command -v automake >/dev/null && command automake --version
    } | head -n1 | cut -f4 -d' '
  )

  local need_to_build_automake=
  if ! is_version_at_least automake "${MINIMUM_AUTOMAKE_VERSION}" echo "${automake_version}"; then
    >&2 echo "automake version lower than ${MINIMUM_AUTOMAKE_VERSION}."
    need_to_build_automake=1
  fi

  if [[ "${need_to_build_automake}" != 1 ]]; then
    >&2 echo "automake rebuild is NOT needed."
    return
  fi

  # Disable and automake116 from Ribose's repository as that may be too old.
  if [[ "${using_ribose_automake}" == 1 ]]; then
    >&2 echo "ribose-automake116 does not meet version requirements, disabling and removing."
    . /opt/ribose/ribose-automake116/disable
    "${SUDO}" rpm -e ribose-automake116
    using_ribose_automake=0
  fi

  >&2 echo "automake rebuild is needed."
  pushd "$(mktemp -d)" || return 1
  build_and_install_automake

  command -v automake
  popd
}

enable_ribose_automake() {
  case "${DIST}" in
    centos|fedora)
      if rpm --quiet -q ribose-automake116 && [[ "$PATH" != */opt/ribose/ribose-automake116/root/usr/bin* ]]; then
        ACLOCAL_PATH=$(scl enable ribose-automake116 -- aclocal --print-ac-dir):$(rpm --eval '%{_datadir}/aclocal')
        export ACLOCAL_PATH
        . /opt/ribose/ribose-automake116/enable
        >&2 echo "Ribose automake was enabled."
        using_ribose_automake=1
      fi
      ;;
  esac
}

enable_llvm_toolset_7() {
  if [[ "${DIST_VERSION}" == "centos-7" ]] && \
     rpm --quiet -q llvm-toolset-7.0 && \
     [[ "$PATH" != */opt/rh/llvm-toolset-7.0/root/usr/bin* ]]; then
    . /opt/rh/llvm-toolset-7.0/enable
  fi
}

enable_rh_ruby30() {
  if [[ "${DIST_VERSION}" == "centos-7" ]] && \
     rpm --quiet -q rh-ruby30 && \
     [[ "$PATH" != */opt/rh/rh-ruby30/root/usr/bin* ]]; then
    . /opt/rh/rh-ruby30/enable
    PATH=$HOME/bin:$PATH
    export PATH
    export SUDO_GEM="run"
  fi
}

build_and_install_automake() {
  # automake
  automake_build=${LOCAL_BUILDS}/automake
  mkdir -p "${automake_build}"
  pushd "${automake_build}"
  curl -L -o automake.tar.xz "https://ftp.gnu.org/gnu/automake/automake-${AUTOMAKE_VERSION}.tar.xz"
  tar -xf automake.tar.xz --strip 1
  ./configure --enable-optimizations --prefix=/usr
  "${MAKE}" -j"${MAKE_PARALLEL}"
  "${SUDO}" "${MAKE}" install
  popd
}

# json-c is installed with install_jsonc
# asciidoctor is installed with install_asciidoctor
linux_install_ubuntu() {
  "${SUDO}" apt-get update
  apt_install \
    "${util_dependencies_ubuntu[@]}" \
    "${basic_build_dependencies_ubuntu[@]}" \
    "${build_dependencies_ubuntu[@]}" \
    "$@"

  ubuntu_install_dynamic_build_dependencies_if_needed
  ensure_automake
}

ubuntu_install_dynamic_build_dependencies_if_needed() {
  if ! is_use_static_dependencies; then
    ubuntu_install_dynamic_build_dependencies
  fi
}

ubuntu_install_dynamic_build_dependencies() {
  apt_install \
    "${dynamic_build_dependencies_ubuntu[@]}"
}

declare util_dependencies_ubuntu=()

declare util_dependencies_deb=(
  sudo
  wget
  git
  software-properties-common
  # botan # Debian 9 does not have botan in default repos?
)

declare basic_build_dependencies_ubuntu=(
  build-essential
  cmake
)

declare basic_build_dependencies_deb=(
  autoconf
  automake
  build-essential
  curl
  libtool
)

declare build_dependencies_ubuntu=(
  gettext
  libbz2-dev
  libncurses-dev
  python3
  python3-venv
  ruby-dev
  zlib1g-dev
)

declare dynamic_build_dependencies_ubuntu=(
  botan
  libbotan-2-dev
)

declare build_dependencies_deb=(
  # botan # Debian 9 does not have botan in default repos?
  gettext
  libbz2-dev
  libncurses5-dev
  libssl-dev
  python3
  python3-venv
  ruby-dev
  zlib1g-dev
)

declare ruby_build_dependencies_ubuntu=(
  bison
  curl
  libbz2-dev
  libssl-dev
  rubygems
  zlib1g-dev
)

declare ruby_build_dependencies_deb=(
  bison
  curl
  libbz2-dev
  libssl-dev
  rubygems
  zlib1g-dev
)

linux_install_debian() {
  "${SUDO}" apt-get update
  apt_install \
    "${util_dependencies_deb[@]}" \
    "${basic_build_dependencies_deb[@]}" \
    "${build_dependencies_deb[@]}" \
    "$@"

  if [ "${CC-gcc}" = "clang" ]; then
# Add apt.llvm.org repository and install clang
# We may use https://packages.debian.org/stretch/clang-3.8 as well but this package gets installed to
# /usr/lib/clang... and requires update-alternatives which would be very ugly considering CC/CXX environment
# settings coming from yaml already
    wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key|sudo apt-key add -
    ${SUDO} apt-add-repository "deb http://apt.llvm.org/stretch/ llvm-toolchain-stretch main"
    ${SUDO} apt-get install -y clang
  fi

  ensure_automake
  ensure_ruby
  ensure_cmake
}

linux_install() {
  if type "linux_install_${DIST}" | grep -qwi 'function'; then
    "linux_install_${DIST}"
  fi
}

msys_install() {
  local packages=(
    tar
    git
    automake
    autoconf
    libtool
    automake-wrapper
    gnupg2
    make
    pkg-config
    p7zip
    mingw64/mingw-w64-x86_64-cmake
    mingw64/mingw-w64-x86_64-python3
  )

  if [ "${CC}" = "gcc" ]; then
    packages+=(mingw64/mingw-w64-x86_64-gcc
               mingw64/mingw-w64-x86_64-libbotan
               mingw64/mingw-w64-x86_64-json-c
    )
  else
    packages+=(clang64/mingw-w64-clang-x86_64-clang
               clang64/mingw-w64-clang-x86_64-openmp
               clang64/mingw-w64-clang-x86_64-libc++
               clang64/mingw-w64-clang-x86_64-libbotan
               clang64/mingw-w64-clang-x86_64-json-c
               clang64/mingw-w64-clang-x86_64-libsystre
    )
  fi

  pacman --noconfirm -S --needed "${packages[@]}"

}

# Mainly for all python scripts with shebangs pointing to
# 'python', which is
# unavailable in CentOS 8 by default.
#
# This creates an environment where straight 'python' is available.
prepare_python_virtualenv() {
  python3 -m venv ~/.venv
}

# Run its arguments inside a python-virtualenv-enabled sub-shell.
run_in_python_venv() {
  if [[ ! -e ~/.venv ]] || [[ ! -f ~/.venv/bin/activate ]]; then
    prepare_python_virtualenv
  fi

  (
    # Avoid issues like '_OLD_VIRTUAL_PATH: unbound variable'
    set +u
    . ~/.venv/bin/activate
    set -u
    "$@"
  )
}

install_asciidoctor() {
  gem_install asciidoctor
}

declare ruby_build_dependencies_yum=(
  zlib
  zlib-devel
  patch
  readline-devel
  libyaml-devel
  libffi-devel
  openssl-devel
  bzip2
  bison
  curl
  sqlite-devel
  which # for rbenv-doctor
)

ensure_ruby() {
  if is_version_at_least ruby "${MINIMUM_RUBY_VERSION}" command ruby -e 'puts RUBY_VERSION'; then
    return
  fi

  if [[ "${DIST_VERSION}" = fedora-20 ]]; then
    ruby_build_dependencies_yum+=(--enablerepo=updates-testing)
  fi

  case "${DIST}" in
    centos|fedora)
      yum_install "${ruby_build_dependencies_yum[@]}"
      setup_rbenv
      rbenv install -v "${RUBY_VERSION}"
      rbenv global "${RUBY_VERSION}"
      rbenv rehash
      "${SUDO}" chown -R "$(whoami)" "$(rbenv prefix)"
      ;;
    debian)
      apt_install "${ruby_build_dependencies_deb[@]}"
      ;;
    ubuntu)
      apt_install "${ruby_build_dependencies_ubuntu[@]}"
      ;;
    *)
      # TODO: handle ubuntu?
      >&2 echo "Error: Need to install ruby ${MINIMUM_RUBY_VERSION}+"
      exit 1
  esac
}


# shellcheck disable=SC2016
setup_rbenv() {
  pushd "$(mktemp -d)" || return 1
  local rbenv_rc=$HOME/setup_rbenv.sh
  git clone https://github.com/sstephenson/rbenv.git ~/.rbenv
  echo 'export PATH="$HOME/.rbenv/bin:$PATH"' >> "${rbenv_rc}"
  echo 'eval "$($HOME/.rbenv/bin/rbenv init -)"' >> "${rbenv_rc}"

  git clone https://github.com/sstephenson/ruby-build.git ~/.rbenv/plugins/ruby-build
  echo 'export PATH="$HOME/.rbenv/plugins/ruby-build/bin:$PATH"' >> "${rbenv_rc}"
  echo ". \"${rbenv_rc}\"" >> ~/.bash_profile
  prepare_rbenv_env

  # Verify rbenv is set up correctly
  curl -fsSL https://github.com/rbenv/rbenv-installer/raw/master/bin/rbenv-doctor | bash
  popd || return 1
}

prepare_rbenv_env() {
  case "${DIST}" in
    centos|fedora)
      local rbenv_rc=$HOME/setup_rbenv.sh
      [[ ! -r "${rbenv_rc}" ]] || . "${rbenv_rc}"
      ;;
  esac

  if command -v rbenv >/dev/null; then
    rbenv rehash
  fi
}

is_version_at_least() {
  local bin_name="${1:?Missing bin name}"; shift
  local version_constraint="${1:?Missing version constraint}"; shift
  local need_to_build=0

  if ! command -v "${bin_name}"; then
    >&2 echo "Warning: ${bin_name} not installed."
    need_to_build=1
  fi

  local installed_version installed_version_major installed_version_minor #version_patch
  installed_version="$("$@")"

  # shellcheck disable=SC2181
  # shellcheck disable=SC2295
  if [[ $? -ne 0 ]]; then
    need_to_build=1
  else
    installed_version_major="${installed_version%%.*}"
    installed_version_minor="${installed_version#*.}"
    installed_version_minor="${installed_version_minor%%.*}"
    installed_version_minor="${installed_version_minor:-0}"
    installed_version_patch="${installed_version#${installed_version_major}.}"
    installed_version_patch="${installed_version_patch#${installed_version_minor}}"
    installed_version_patch="${installed_version_patch#[.-]}"
    installed_version_patch="${installed_version_patch%%[.-]*}"
    installed_version_patch="${installed_version_patch:-0}"

    local need_version_major
    need_version_major="${version_constraint%%.*}"
    need_version_minor="${version_constraint#*.}"
    need_version_minor="${need_version_minor%%.*}"
    need_version_minor="${need_version_minor:-0}"
    need_version_patch="${version_constraint##*.}"
    need_version_patch="${version_constraint#${need_version_major}.}"
    need_version_patch="${need_version_patch#${need_version_minor}}"
    need_version_patch="${need_version_patch#.}"
    need_version_patch="${need_version_patch%%.*}"
    need_version_patch="${need_version_patch:-0}"

    # Naive semver comparison
    if [[ "${installed_version_major}" -lt "${need_version_major}" ]] || \
       [[ "${installed_version_major}" = "${need_version_major}" && "${installed_version_minor}" -lt "${need_version_minor}" ]] || \
       [[ "${installed_version_major}.${installed_version_minor}" = "${need_version_major}.${need_version_minor}" && "${installed_version_patch}" -lt "${need_version_patch}" ]]; then
      need_to_build=1
    fi
  fi

  if [[ 1 = "${need_to_build}" ]]; then
    >&2 echo "Warning: Need to build ${bin_name} since version constraint ${version_constraint} not met."
  else
    >&2 echo "No need to build ${bin_name} since version constraint ${version_constraint} is met."
  fi

  return "${need_to_build}"
}

# Install specified gem.
# Use rbenv when available.  Otherwise use system 'gem', and use 'sudo'
# depending on OS.
# Set SUDO_GEM to 'sudo' to force use of sudo.
# Set SUDO_GEM to 'run' to disable sudo.
gem_install() {
  local gem_name="${1:?Missing gem name}"
  local bin_name="${2:-${gem_name}}"
  if ! command -v "${bin_name}" >/dev/null; then
    if command -v rbenv >/dev/null; then
      gem install "${gem_name}"
      rbenv rehash
    else
      "${SUDO_GEM:-${SUDO:-run}}" gem install "${gem_name}"
    fi
  fi
}

build_rnp() {
# shellcheck disable=SC2154
  "${CMAKE:-cmake}" "${cmakeopts[@]}" "${1:-.}"
}

make_install() {
  make -j"${MAKE_PARALLEL}" install "$@"
}

is_true_cmake_bool() {
  local arg="${1:?Missing parameter}"
  case "${arg}" in
    yes|on|true|y)
      true
      ;;
    no|off|false|n)
      false
      ;;
    *)
      >&2 echo "Warning: unrecognized boolean expression ($arg).  Continuing and interpreting as 'false' anyway."
      false
  esac
}
