#!/usr/bin/env bash
# shellcheck disable=SC1091
set -euxo pipefail

: "${BUILD_SHARED_LIBS:=off}"

: "${VERBOSE:=1}"

declare packaging_dependencies_yum=(
  rpmdevtools
  )

install_rpm_packaging_utils() {
  yum_install \
    "${packaging_dependencies_yum[@]}" \
    "$@"
}

install_packaging_dependencies() {
  case "${DIST}" in
    centos)
      install_rpm_packaging_utils epel-rpm-macros
      ;;
    *)
      install_rpm_packaging_utils
  esac
}

# NOTE: This should be done by install_noncacheable_dependencies.sh.
install_build_dependencies() {
  "${OS}_install"
}

install_dependencies() {
  # NOTE: This is done by install_noncacheable_dependencies.sh.
  # install_build_dependencies
  install_packaging_dependencies
}

prepare_build_package() {
  install_dependencies
  rpmdev-setuptree
  export SOURCE_PATH=rnp${RNP_VERSION:+-${RNP_VERSION}}
  cp -a "${GITHUB_WORKSPACE}" ~/rpmbuild/SOURCES/"${SOURCE_PATH}"
}

build_package() {
  pushd ~/rpmbuild/SOURCES/"${SOURCE_PATH}"

  # XXX: debug
  command -v asciidoctor

  cpack -G RPM --config ./CPackSourceConfig.cmake
  make package VERBOSE="${VERBOSE}"

  popd
}

post_build_package() {
  pushd ~/rpmbuild/SOURCES/"${SOURCE_PATH}"

  mv ./*.src.rpm ~/rpmbuild/SRPMS/
  # mkdir -p ~/rpmbuild/RPMS/noarch/
  # mv *.noarch.rpm ~/rpmbuild/RPMS/noarch/
  mkdir -p ~/rpmbuild/RPMS/x86_64/
  mv ./*.rpm ~/rpmbuild/RPMS/x86_64/

  popd
}

test_packages() {
  yum_install ~/rpmbuild/RPMS/x86_64/*.rpm
}

main() {
  # For asciidoctor:
  export PATH=$HOME/bin:$PATH

  . ci/env.inc.sh

  prepare_build_package

  export LDFLAGS='-Wl,-t' # XXX: DELETEME: for debugging only

  pushd ~/rpmbuild/SOURCES/"${SOURCE_PATH}"

  export cmakeopts=(
    -DBUILD_SHARED_LIBS="${BUILD_SHARED_LIBS}"
    -DBUILD_TESTING=no
    -DCPACK_GENERATOR=RPM
  )
  build_rnp "."
  popd

  build_package
  post_build_package
  test_packages
}

main "$@"
