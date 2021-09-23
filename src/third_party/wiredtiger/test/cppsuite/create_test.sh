#! /bin/bash

# First argument needs to be the name of the script.
if [ $# -eq 0 ]
  then
    echo "Please give a name to your test i.e ./s_new_test my_test"
    exit 128
fi

# Check the test name
if [[ $1 =~ ^[0-9a-zA-Z_-]+$ ]];then
    echo "Generating test: $1..."
else
    echo "Invalid test name. Only alphanumeric characters are allowed. \"_\" and \"-\" can be used too."
    exit 128
fi

# Check if the test already exists.
FILE=tests/$1.cxx
if test -f "$FILE"; then
    echo "$FILE cannot be created as it already exists."
    exit 1
fi

# Check if default configuration associated to the test already exists.
CONFIG=configs/$1_default.txt
if test -f "$CONFIG"; then
    echo "$CONFIG cannot be created as it already exists."
    exit 1
fi

# Copy the default template.
cp tests/example_test.cxx "$FILE"
echo "Created $FILE."
cp configs/example_test_default.txt "$CONFIG"
echo "Created $CONFIG."

# Replace example_test with the new test name.
SEARCH="example_test"
sed -i "s/$SEARCH/$1/" "$FILE"
echo "Updated $FILE."

# Replace the first line of the configuration file.
REPLACE="# Configuration for $1."
sed -i "1s/.*/$REPLACE/" "$CONFIG"
echo "Updated $CONFIG."

# Include the new test in run.cxx
FILE=tests/run.cxx
SEARCH="#include \"example_test.cxx\""
VALUE="#include \"$1.cxx\""
sed -i "/$SEARCH/a $VALUE" $FILE

# Add the new test to the run_test() method
SEARCH="example_test("
LINE_1="\else if (test_name == \"$1\")\n"
LINE_2="\ $1(test_harness::test_args{config, test_name, wt_open_config}).run();"
sed -i "/$SEARCH/a $LINE_1$LINE_2" $FILE

# Add the new test to all existing tests.
SEARCH="all_tests = {"
REPLACE="$SEARCH\"$1\", "
sed -i "s/$SEARCH/$REPLACE/" $FILE
echo "Updated $FILE."

# Add the new test to test_data.py
FILE=../../dist/test_data.py
SEARCH="example_test"
LINE_1="\    '$1' : Method(test_config),"
sed -i "/$SEARCH/a $LINE_1" $FILE
echo "Updated $FILE."

# Trigger s_all
echo "Running s_all.."
cd ../../dist || exit 1
./s_all

# Last changes to be done manually
echo "Follow the next steps to execute your new test:"
echo "1. Start editing $1.cxx"
echo "2. Compile your changes, go to <build_dir>/test/cppsuite and run your test with ./run -t $1"
