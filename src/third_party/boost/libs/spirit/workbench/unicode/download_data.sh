#! /usr/bin/env bash

# Copyright (c) 2022 Mikalai Ananenka
#
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) 

usage()
{
    cat <<EOF
Usage:
$THIS_SCRIPT <outdir>

This script downloads Unicode data files required to generate Unicode tables.

EOF
}

set -e

THIS_SCRIPT="$(basename "$0")"

if [[ $# -ne 1 ]]; then
    usage
    echo "${THIS_SCRIPT}: expected one argument but got $#" >&2
    exit 1

elif [[ "-h" == "$1" || "--help" == "$1" ]]; then
    usage

else
    which curl > /dev/null
    cd "$1"

    UNICODE_VERSION="15.0.0"
    BASE_URL="https://unicode.org/Public/${UNICODE_VERSION}/ucd/"

    echo "downloading data files for Unicode $UNICODE_VERSION ..."

    curl -O "${BASE_URL}{DerivedCoreProperties,PropList,Scripts,UnicodeData}.txt"

    echo -e "\ndone. Now you can run create_tables executable in directory '$1'"
fi
