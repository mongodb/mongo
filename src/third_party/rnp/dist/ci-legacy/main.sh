#!/usr/bin/env bash
set -eux

. ci/env.inc.sh

: "${GPG_VERSION:=stable}"
: "${BUILD_MODE:=normal}"

: "${RNP_TESTS=${RNP_TESTS-.*}}"
: "${LD_LIBRARY_PATH:=}"

: "${DIST:=}"
: "${DIST_VERSION:=}"

: "${SKIP_TESTS:=0}"

prepare_build_prerequisites() {
  CMAKE=cmake

  case "${OS}-${CPU}" in
#    linux-i386)                                       #  For i386/Debian (the only 32 bit run) python3 is already installed by
#      build_and_install_python                        #  linux_install @ install_functions.inc.sh called from install_cacheable_dependencies.sh
#      ;;                                              #  If there is a distribution that does not have python3 pre-apckeges (highly unlikely)
    linux-*)                                           #  it shall be implemented like ensure_cmake
      ensure_cmake
      ;;
  esac

  export CMAKE
}

prepare_test_env() {
  prepare_build_tool_env

  export LD_LIBRARY_PATH="${GPG_INSTALL}/lib:${BOTAN_INSTALL}/lib:${JSONC_INSTALL}/lib:${RNP_INSTALL}/lib:$LD_LIBRARY_PATH"

  # update dll search path for windows
  if [[ "${OS}" = "msys" ]]; then
    export PATH="${LOCAL_BUILDS}/rnp-build/lib:${LOCAL_BUILDS}/rnp-build/bin:${LOCAL_BUILDS}/rnp-build/src/lib:${BOTAN_INSTALL}/bin:$PATH"
  fi
}

prepare_tests() {
  : "${COVERITY_SCAN_BRANCH:=0}"
  if [[ ${COVERITY_SCAN_BRANCH} = 1 ]]; then
    exit 0
  fi
}

build_tests() {
  #  use test costs to prioritize
  mkdir -p "${LOCAL_BUILDS}/rnp-build/Testing/Temporary"
  cp "${rnpsrc}/cmake/CTestCostData.txt" "${LOCAL_BUILDS}/rnp-build/Testing/Temporary"

  local run=run
  case "${DIST_VERSION}" in
    centos-8|centos-9|fedora-*)
      run=run_in_python_venv
      ;;
  esac

  "${run}" ctest -j"${CTEST_PARALLEL}" -R "$RNP_TESTS" --output-on-failure
  popd
}

main() {
  if [[ ${SKIP_TESTS} = 0 ]]; then
    prepare_test_env
  fi
  prepare_build_prerequisites

  export rnpsrc="$PWD"

  mkdir -p "${LOCAL_BUILDS}/rnp-build"
  pushd "${LOCAL_BUILDS}/rnp-build"

  cmakeopts=(
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_SHARED_LIBS=yes
    -DCMAKE_INSTALL_PREFIX="${RNP_INSTALL}"
    -DCMAKE_PREFIX_PATH="${BOTAN_INSTALL};${JSONC_INSTALL};${GPG_INSTALL}"
  )
  [[ ${SKIP_TESTS} = 1 ]] && cmakeopts+=(-DBUILD_TESTING=OFF)
  [[ "${BUILD_MODE}" = "coverage" ]] && cmakeopts+=(-DENABLE_COVERAGE=yes)
  [[ "${BUILD_MODE}" = "sanitize" ]] && cmakeopts+=(-DENABLE_SANITIZERS=yes)
  [ -n "${GTEST_SOURCES:-}" ] && cmakeopts+=(-DGTEST_SOURCES="${GTEST_SOURCES}")
  [ -n "${DOWNLOAD_GTEST:-}" ] && cmakeopts+=(-DDOWNLOAD_GTEST="${DOWNLOAD_GTEST}")
  [ -n "${CRYPTO_BACKEND:-}" ] && cmakeopts+=(-DCRYPTO_BACKEND="${CRYPTO_BACKEND}")
  [ -n "${ENABLE_SM2:-}" ] && cmakeopts+=(-DENABLE_SM2="${ENABLE_SM2}")
  [ -n "${ENABLE_IDEA:-}" ] && cmakeopts+=(-DENABLE_IDEA="${ENABLE_IDEA}")
  [ -n "${ENABLE_TWOFISH:-}" ] && cmakeopts+=(-DENABLE_TWOFISH="${ENABLE_TWOFISH}")
  [ -n "${ENABLE_BRAINPOOL:-}" ] && cmakeopts+=(-DENABLE_BRAINPOOL="${ENABLE_BRAINPOOL}")

  if [[ "${OS}" = "msys" ]]; then
    cmakeopts+=(-G "MSYS Makefiles")
  fi
  build_rnp "${rnpsrc}"
  make_install VERBOSE=0

  if [[ ${SKIP_TESTS} = 0 ]]; then
    echo "TESTS NOT SKIPPED"
    prepare_tests
    build_tests
  fi
}

main "$@"

exit 0
