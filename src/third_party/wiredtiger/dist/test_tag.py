import os
import sys
from dist import compare_srcfile

##### LOCAL VARIABLES AND CONSTANTS #####
show_info = False
show_missing_files = False

nb_ignored_files = 0
nb_missing_files = 0
nb_valid_files = 0

sorted_tags = []
test_files = []

tagged_files = {}
valid_tags = []
test_type_tags = ['data_correctness', 'functional_correctness']

C_FILE_TYPE = 0
PY_FILE_TYPE = 1
END_TAG = "[END_TAGS]"
IGNORE_FILE = "ignored_file"
START_TAG = "[TEST_TAGS]"
#####

##### FUNCTIONS #####
def validate_tag(tag, filename):
    split_tag = tag.split(":")
    # Ensure the array isn't too long.
    if (len(split_tag) > 3):
        print("Tag contains too many sub tags: " + tag + " filename: " + filename);
        exit(1)

    # Walk the pieces of the tag and ensure they exist in test_tags.ok.
    for sub_tag in split_tag:
        if not sub_tag in valid_tags:
            print(
                "Invalid sub tag found: " + sub_tag + " in tag: " + tag + " filename: " + filename)
            exit(1)

def format_tag(tag):
    return tag.replace("_", " ").title()
#####

##### PROCESS ARGS #####
for arg in sys.argv:
    if arg == "-h":
        print("Usage: python test_tag.py [options]")
        print("Options:")
        print("\t-i\tShow info")
        print("\t-p\tShow files with no test tags")
        exit(1)
    elif arg == "-i":
        show_info = True
    elif arg == "-p":
        show_missing_files = True
#####

##### GET ALL TEST FILES #####
for root, dirs, files in os.walk("../test/"):
    path = root.split(os.sep)
    for file in files:
        filename = os.path.join('/'.join(path), file)
        if filename.endswith("main.c") or filename.endswith(".py"):
            test_files.append(filename)
#####

##### RETRIEVE VALID TAGS #####
validation_file = open("test_tags.ok", "r")

# The validation file contains an alphabetized list of valid tag words, with one word per line.
tags = validation_file.readlines()
tags = [tag.replace('\n', '') for tag in tags]

for tag in tags:
    valid_tags.append(tag)

# Check that the validation file is ordered.
if not all(tags[i] <= tags[i+1] for i in range(len(tags)-1)):
    print("Error: test_tags.ok is not alphabetically ordered!")
    exit(1)

validation_file.close()
#####

##### PARSE TEST FILES #####
for filename in test_files:
    input_file = open(filename, "r")
    lines = input_file.readlines()

    in_tag_block = False
    is_file_ignored = False
    is_file_tagged = False

    # Read line by line
    for line in lines:
        # Format line
        line = line.replace('\n', '').replace('\r', '') \
                   .replace(' ', '').replace('#', '') \
                   .replace('*', '')

        # Check if line is valid
        if not line:
            # Check if invalid line after START_TAG
            if in_tag_block == True:
                print("Syntax error in file: " + filename)
                exit(1)
            else:
                continue

        # Check if end of test tag
        if END_TAG in line:
            # END_TAG should not be before START_TAG
            if in_tag_block == False:
                print("Syntax error in file: " + filename + ". Unexpected tag: " + END_TAG)
                exit(1)
            # END_TAG should not be met before a test tag
            if is_file_ignored == False and is_file_tagged == False:
                print("Syntax error in file: " + filename + ". Missing test tag.")
                exit(1)
            nb_valid_files = nb_valid_files + 1
            # Go to next file
            break

        # Check if start of test tag
        if START_TAG in line:
            # Only one START_TAG is allowed
            if in_tag_block == True:
                print("Syntax error in file: " + filename + ". Unexpected tag: " + START_TAG)
                exit(1)
            in_tag_block = True
            continue

        if in_tag_block == True:
            tag = line
            # Check if file is ignored
            if is_file_ignored == True:
                print("Unexpected value in ignored file: " + filename)
                exit(1)
            if tag == IGNORE_FILE:
                nb_ignored_files = nb_ignored_files + 1
                is_file_ignored = True
                continue

            # Validate the tag's correctness.
            validate_tag(tag, filename)
            is_file_tagged = True

            skip_adding_test_type = False
            # Add the test type to the tag if it wasn't already.
            for test_type_tag in test_type_tags:
                if (tag.startswith(test_type_tag)):
                    skip_adding_test_type = True
                    break

            if not skip_adding_test_type:
                if filename.endswith(".c"):
                    tag = test_type_tags[C_FILE_TYPE] + ":" + tag
                if filename.endswith(".py"):
                    tag = test_type_tags[PY_FILE_TYPE] + ":" + tag

            # Check if current tag has already matched test files
            if tag in tagged_files:
                tagged_files[tag].append(filename)
            else:
                tagged_files[tag] = [filename]

    if is_file_ignored == False and is_file_tagged == False:
        nb_missing_files = nb_missing_files + 1
        if show_missing_files == True:
            print("Missing test tag in file: " + filename)

    input_file.close()
#####

##### GENERATE TEST COVERAGE MD #####
tmp_filename = '__tmp'
tfile = open('__tmp', 'w')

# Sort tags
sorted_tags = list(tagged_files.keys())
sorted_tags.sort()

for test_type_tag in test_type_tags:
    tfile.write("## " + format_tag(test_type_tag) + " tests:\n\n")
    tfile.write("|Component|Sub-component|Existing tests|\n")
    tfile.write("|---|---|---|\n")

    for tag in sorted_tags:
        # Split the tag.
        current_tag = tag.split(":")
        if (current_tag[0] != test_type_tag):
            continue

        # Parse and format tag.
        component = format_tag(current_tag[1])
        functionality = ""

        # The end tag is optional.
        if (len(current_tag) == 3):
            functionality = format_tag(current_tag[2])

        # Relative path to test files
        link = ""
        # Sort the filenames associated to the current tag
        tagged_files[tag].sort()
        for name in tagged_files[tag]:
            link += "[" + os.path.basename(name) + "](" + name + "), "
        # Remove the extra ", " at the end
        link = link[:-2]

        # Write to output
        tfile.write('|' + component + '|' + \
                        functionality + '|' + link + '\n')
tfile.close()
compare_srcfile(tmp_filename, "../test/test_coverage.md")
#####

##### STATS #####
if show_info == True:
    print("Tagged files:\t" + str(nb_valid_files - nb_ignored_files))
    print("Missing files:\t" + str(nb_missing_files))
    print("Ignored files:\t" + str(nb_ignored_files))
    print("Total files:\t" + str(nb_valid_files + nb_missing_files))
#####

# Enforce tagging
#if nb_missing_files > 0:
#    print("Files missing a tag: " + str(nb_missing_files))
#    if show_missing_files == False:
#        print("Call \'python test_tag.py -p\' to list all files with no tags")
