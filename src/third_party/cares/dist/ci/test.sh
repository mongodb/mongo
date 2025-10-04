#!/bin/sh
# Copyright (C) The c-ares project and its contributors
# SPDX-License-Identifier: MIT
set -e

# Travis on MacOS uses CloudFlare's DNS (1.1.1.1/1.0.0.1) which rejects ANY requests.
# Also, LiveSearchTXT is known to fail on Cirrus-CI on some MacOS hosts, we don't get
# a truncated UDP response so we never follow up with TCP.
# Note res_ninit() and /etc/resolv.conf actually have different configs, bad Travis
[ -z "$TEST_FILTER" ] && export TEST_FILTER="--gtest_filter=-*LiveSearchTXT*:*LiveSearchANY*"

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

if [ "$BUILD_TYPE" = "autotools" -o "$BUILD_TYPE" = "coverage" ]; then
    TOOLSBIN="${PWD}/atoolsbld/src/tools"
    TESTSBIN="${PWD}/atoolsbld/test"
else
    TOOLSBIN="${PWD}/cmakebld/bin"
    TESTSBIN="${PWD}/cmakebld/bin"
fi

$TEST_WRAP "${TOOLSBIN}/adig" www.google.com
$TEST_WRAP "${TOOLSBIN}/ahost" www.google.com
cd "${TESTSBIN}"
$TEST_WRAP ./arestest -4 -v $TEST_FILTER
./aresfuzz ${TESTDIR}/fuzzinput/*
./aresfuzzname ${TESTDIR}/fuzznames/*
./dnsdump "${TESTDIR}/fuzzinput/answer_a" "${TESTDIR}/fuzzinput/answer_aaaa"
cd "${PWD}"
