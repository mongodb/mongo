#!/bin/sh
set -e

# Travis on MacOS uses CloudFlare's DNS (1.1.1.1/1.0.0.1) which rejects ANY requests
# Note res_ninit() and /etc/resolv.conf actually have different configs, bad Travis
[ -z "$TEST_FILTER" ] && export TEST_FILTER="--gtest_filter=-*LiveSearchANY*"

# No tests for ios as it is a cross-compile
if [ "$BUILD_TYPE" = "ios" -o "$BUILD_TYPE" = "ios-cmake" -o "$DIST" = "iOS" ] ; then
    exit 0
fi

# Analyze tests don't need runtime, its static analysis
if [ "$BUILD_TYPE" = "analyze" ] ; then
    exit 0
fi

PWD=`pwd`
TESTDIR="${PWD}/test"

if [ "$BUILD_TYPE" = "cmake" -o "$BUILD_TYPE" = "valgrind" ] ; then
    TOOLSBIN="${PWD}/cmakebld/bin"
    TESTSBIN="${PWD}/cmakebld/bin"
else
    TOOLSBIN="${PWD}/atoolsbld/src/tools"
    TESTSBIN="${PWD}/atoolsbld/test"
fi

$TEST_WRAP "${TOOLSBIN}/adig" www.google.com
$TEST_WRAP "${TOOLSBIN}/acountry" www.google.com
$TEST_WRAP "${TOOLSBIN}/ahost" www.google.com
cd "${TESTSBIN}"
$TEST_WRAP ./arestest -4 -v $TEST_FILTER
./aresfuzz ${TESTDIR}/fuzzinput/*
./aresfuzzname ${TESTDIR}/fuzznames/*
./dnsdump "${TESTDIR}/fuzzinput/answer_a" "${TESTDIR}/fuzzinput/answer_aaaa"
cd "${PWD}"
