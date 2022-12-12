#! /bin/bash

# First argument needs to be the name of the script.
if [ $# -eq 0 ]
  then
    echo "Please give a name to your test i.e ./s_new_test my_test"
    exit 128
fi

# Check the test name
if [[ $1 =~ ^[a-z][_a-z0-9]+$ ]]; then
    echo "Generating test: $1..."
else
    echo "Invalid test name. Test name must match the regex '[a-z][_a-z0-9]+$'"
    exit 128
fi

# Check if the test already exists.
FILE=tests/$1.cpp
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
cp tests/test_template.cpp "$FILE"
echo "Created $FILE."
cp configs/test_template_default.txt "$CONFIG"
echo "Created $CONFIG."

# Replace test_template with the new test name.
SEARCH="test_template"
sed -i "s/$SEARCH/$1/" "$FILE"
echo "Updated test name in $FILE."

# Replace operation_tracker_template with the new tracking table name.
SEARCH="operation_tracker_template"
sed -i "s/$SEARCH/operation_tracker_$1/" "$FILE"
echo "Updated tracking table name in $FILE."

# Replace the first line of the configuration file.
REPLACE="# Configuration for $1."
sed -i "1s/.*/$REPLACE/" "$CONFIG"
echo "Updated $CONFIG."

# Include the new test in run.cpp
FILE=tests/run.cpp
SEARCH="#include \"test_template.cpp\""
VALUE="#include \"$1.cpp\""
sed -i "/$SEARCH/a $VALUE" $FILE

# Add the new test to the run_test() method
SEARCH="test_template("
LINE_1="\else if (test_name == \"$1\")\n"
LINE_2="\ $1(args).run();"
sed -i "/$SEARCH/a $LINE_1$LINE_2" $FILE

# Add the new test to all existing tests.
SEARCH="all_tests = {"
REPLACE="$SEARCH\"$1\", "
sed -i "s/$SEARCH/$REPLACE/" $FILE
echo "Updated $FILE."

# Add the new test to test_data.py
FILE=../../dist/test_data.py
SEARCH="test_template"
LINE_1="\    '$1' : Method(test_config),"
sed -i "/$SEARCH/a $LINE_1" $FILE
echo "Updated $FILE."

# Trigger s_all
echo "Running s_all.."
cd ../../dist || exit 1
./s_all

# Last changes to be done manually
echo "Follow the next steps to execute your new test:"
echo "1. Start editing $1.cpp"
echo "2. Compile your changes, go to <build_dir>/test/cppsuite and run your test with ./run -t $1"
