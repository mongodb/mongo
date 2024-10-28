#!/bin/bash
# create_test.sh: Create a boilerplate template for new unit tests. The script will create the new
# template based on the argument name and add a reference to the file inside the CMakeLists.txt.
#
INCLUDE_SESSION_DIR="wrappers/mock_session.h"

usage_message="Usage: ./create_test.sh [-m module] my_test"
if [[ $# -eq 0 || $# -gt 3 ]]; then
    echo $usage_message
    exit 128
elif [[ $1 == "-m" && $# -ne 3 ]]; then
    echo $usage_message
    exit 128
elif [[ $1 == "-m" && $# -eq 1 ]]; then
    echo $usage_message
    exit 128
fi

# If configured, grab the test directory name.
module_dir=""
if [[ $1 == "-m" ]]; then
    module_dir="$2"
    shift ; shift ;
fi

# Check the test name.
if [[ $1 =~ ^[a-z][_a-z0-9]+$ ]]; then
    echo "Generating test: $1..."
else
    echo "Invalid test name. Test name $1 must match the regex '[a-z][_a-z0-9]+$'"
    exit 128
fi

# Check if the test already exists in the top level test and modules directory. If module
# directory is set, the file path will be set to the module directory.
file="$1.cpp"
directory="tests"
if test -f "$directory/$file"; then
    echo "$file cannot be created as it already exists in the top level test directory."
    exit 1
fi

if [ -n "$module_dir" ]; then
    directory="tests/$module_dir"
    INCLUDE_SESSION_DIR="../$INCLUDE_SESSION_DIR"
    if test -f "$directory/$file"; then
        echo "$file cannot be created as it already exists in the module directory."
        exit 1
    fi
fi
file_path="$directory/$file"

# Copy the default template.
TEMPLATE=$(cat <<- EOF
/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wt_internal.h"
#include "${INCLUDE_SESSION_DIR}"
/*
 * [test_template]: source_file.c
 * Add comment describing the what the unit test file tests.
 */

/*
 * Each test case should be testing one wiredtiger function. The test case description should be in
 * format "[Module]: Function [Tag]".
 */
TEST_CASE("Boilerplate: test function", "[test_template]")
{
    /*
     * Build Mock session, this will automatically create a mock connection. Remove if not
     * necessary.
     */
    std::shared_ptr<mock_session> session = mock_session::build_test_mock_session();

    /*
     * Sections are a great way to separate the different edge cases for a particular function. In
     * sections description, describe what edge case is being tested. Remove if not necessary.
     */
    SECTION("Boilerplate test section") {}
}
EOF

)
echo "$TEMPLATE" > $file_path
echo "Created $file_path."

# Grab the list of unit test files and sort alphabetically.
sorted_array=($(cat CMakeLists.txt | sed -E 's/^\s+//' | grep "^$directory/test_" | sort))

# Find the unit test file that we are planning to append under using sed command.
sorted_file_name=""
for s in "${sorted_array[@]}"
do
    sorted_file_name=$s
    if [[ $file_path < $s ]]; then
        break
    fi
done

# Escape all the "/" in the unit test file path, so that we can use sed command.
escaped_file_name=$(echo "$sorted_file_name" | sed 's/\//\\\//g')

# Add the new test to the CMakeLists.txt
file_entry="\        $file_path"
sed -i "/$escaped_file_name/a $file_entry" CMakeLists.txt

# Trigger s_all
echo "Running s_all.."
cd ../../dist || exit 1
./s_all

# Last changes to be done manually
echo "Follow the next steps to execute your new test:"
echo "1. Start editing $1.cpp"
echo "2. Compile your changes, run the unit test via test/catch2/catch2-unittests [tag_name]"
