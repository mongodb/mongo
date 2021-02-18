import os
import sys

##### LOCAL VARIABLES AND CONSTANTS #####
component = ""
testing_area = ""
test_type = ""

is_end = False
is_file_ignored = False
is_file_tagged = False
is_start = False
show_info = False
show_missing_files = False

nb_ignored_files = 0
nb_missing_files = 0
nb_valid_files = 0

sorted_tags = []
test_files = []

tagged_files = {}
valid_tags = {}

END_TAG = "[END_TAGS]"
IGNORE_FILE = "ignored_file"
NB_TAG_ARGS = 3
START_TAG = "[TEST_TAGS]"
#####

##### PROCESS ARGS #####
for arg in sys.argv:
    if arg == "-h":
        print("Usage: python test_tag.py [options]")
        print("Options:")
        print("\t-i\tShow info")
        print("\t-p\tShow files with no test tags")
        exit()
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

# The file has the following pattern
# <COMPONENT>:<TESTING_TYPE>:<TESTING_AREA>:<DESCRIPTION>
# A tag is made of the three first values: COMPONENT, TEST_TYPE and TESTING_AREA
tags = validation_file.readlines()
tags = [tag.replace('\n', '') for tag in tags]

for tag in tags:
    current_line = tag.split(':')
    # Createa key value pair <TAG>:<DESCRIPTION>
    valid_tags[':'.join(current_line[:NB_TAG_ARGS])] = ':'.join(current_line[NB_TAG_ARGS:])

validation_file.close()
#####

##### PARSE TEST FILES #####
for filename in test_files:
    input_file = open(filename, "r")
    lines = input_file.readlines()

    is_start = False
    is_end = False
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
            if is_start == True:
                print("Error syntax in file " + filename)
                exit()
            else:
                continue

        # Check if end of test tag
        if END_TAG in line:
            # END_TAG should not be before START_TAG
            if is_start == False:
                print("Error syntax in file " + filename + ". Unexpected tag: " + END_TAG)
                exit()
            # END_TAG should not be met before a test tag
            if is_file_ignored == False and is_file_tagged == False:
                print("Error syntax in file " + filename + ". Missing test tag.")
                exit()
            is_end = True
            nb_valid_files = nb_valid_files + 1
            # Go to next file
            break

        # Check if start of test tag
        if START_TAG in line:
            # Only one START_TAG is allowed
            if is_start == True:
                print("Error syntax in file " + filename + ". Unexpected tag: " + START_TAG)
                exit()
            is_start = True
            continue

        if is_start == True:
            # Check if file is ignored
            if is_file_ignored == True:
                print("Unexpected value in ignored file: " + filename)
                exit()
            if line == IGNORE_FILE:
                nb_ignored_files = nb_ignored_files + 1
                is_file_ignored = True
                continue
            # Check if current tag is valid
            if not line in valid_tags:
                print("Tag is not valid ! Add the new tag to test_tags.ok:\n" + line)
                exit()
            else:
                is_file_tagged = True
            # Check if current tag has already matched test files
            if line in tagged_files:
                tagged_files[line].append(filename)
            else:
                tagged_files[line] = [filename]

    if is_file_ignored == False and is_file_tagged == False:
        nb_missing_files = nb_missing_files + 1
        if show_missing_files == True:
            print("Missing test tag in file: " + filename)

    input_file.close()
#####

##### GENERATE OUTPUT #####
output_file = open("../test/test_coverage.md", "w")

# Table headers
output_file.write("|Component|Test Type|Testing Area|Description|Existing tests|" + '\n')
output_file.write("|---|---|---|---|---|" + '\n')

# Sort tags
sorted_tags = list(tagged_files.keys())
sorted_tags.sort()

for tag in sorted_tags:
    # Split line
    current_line = tag.split(":")

    # Parse tag
    component = current_line[0]
    test_type = current_line[1]
    testing_area = current_line[2]

    # Format output
    component = component.replace("_", " ").title()
    test_type = test_type.replace("_", " ").title()
    testing_area = testing_area.replace("_", " ").title()

    # Relative path to test files
    link = ""
    # Sort the filenames associated to the current tag
    tagged_files[tag].sort()
    for name in tagged_files[tag]:
        link += "[" + name + "](" + name + "), "
    # Remove the extra ", " at the end
    link = link[:-2]

    # Write to output
    output_file.write('|' + component + '|' + test_type + '|' + \
                     testing_area + '|' + valid_tags[tag] + '|' \
                     + link + '\n')

output_file.close()
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
