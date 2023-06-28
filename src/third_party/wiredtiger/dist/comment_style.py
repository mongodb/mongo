#!/usr/bin/env python3

import argparse, os, re, subprocess, sys

def report_illegal_comment(file_name, line_num, line, multiline):
    print("Illegal " + multiline + "comment in " + file_name + ":" + str(line_num - 1) + " " +
        line.strip('\n'))

def check_c_comments(file_name):
    count = 0
    with open(file_name) as f:
        line_num = 0
        for line in f:
            stripped = line.strip()
            if has_single_line_comment(stripped):
                report_illegal_comment(file_name, line_num, line, "")
                count +=1
            line_num += 1
    return count

def has_single_line_comment(stripped_line):
    # We don't worry about whitespace in matches as we strip the line of whitespace.
    # URLs are a false positive that we want to ignore.
    url = re.compile(r'https?:\/\/')

    # // Style comments. The gist of it is: match anything except certain
    # combinations of characters before a //. The trick is in the "certain combinations
    # of characters" part, which consists of an odd number of un-escaped string
    # markers, while ignoring everything else. As an aside, the "?:" notation indicates
    # a non-capturing group, and speeds up the script considerably since we're saving
    # any captures.
    single_line_comment = re.compile(r'^(?:[^"\\]|"(?:[^"\\]|\\.)*"|\\.)*//(?:.*)$')

    return single_line_comment.match(stripped_line) and not url.search(stripped_line)

def check_cpp_comments(file_name):
    # Text before comments
    text_check = re.compile(r'^[^\/\n]+')

    with open(file_name) as f:
        count = 0
        line_num = 0
        length = 0
        for line in f:
            stripped = line.strip()
            if has_single_line_comment(stripped):
                # Detecting multi-line comments of // is not easy as technically they can occur
                # on contiguous lines without being multi-line.
                # E.g:
                # int height; // the height of our object
                # int width; // the width of our object
                # So we can't just count the number of lines we need to check that there is no text
                # preceding the comment in subsequent lines.
                if length != 0 and not text_check.match(line.strip()):
                    # If the comment is length 2 and we found another matching line then we have
                    # found an illegal comment.
                    if length == 2:
                        report_illegal_comment(file_name, line_num, line, "multiline ")
                        count += 1
                    length += 1
                    # Try and print only one error per comment, just keep incrementing the count and
                    # the above if will only run once.
                else:
                    length += 1
            else:
                length = 0
            line_num += 1
        return count

def file_is_cpp(name):
    if re.search('(.cpp|.hpp)$', name) is not None:
        return True
    if re.search('(.c|.i|.in)$', name) is not None:
        return False

    # House style is that C++ header files use ".h", which unfortunately makes
    # this sort of code rather difficult. Luckily, libmagic can identify C/C++
    # based on content. Don't import it because Python packaging is a disaster
    # and this script needs to run reliably.
    result = subprocess.run("file {}".format(name),
                            shell=True,
                            capture_output=True,
                            text=True).stdout.strip('\n')
    return "C++" in result

# A collection of cases we handle.
tests = [
    (r'// hello world', True),
    (r'     // hello world', True),
    (r'hello world', False),
    (r'printf("Hello, World!\n"); // prints hello world', True),
    (r'String url = "http://www.example.com"', False),
    (r'// hello world', True),
    (r'//\\', True),
    (r'// "some comment"', True),
    (r'new URI("http://www.google.com")', False),
    (r'printf("Escaped quote\""); // Comment', True),
    (r' * http://www.google.com', False)
]

def validate(line, expect_match):
    if (expect_match and not has_single_line_comment(line.strip())) \
        or (not expect_match and has_single_line_comment(line.strip())):
        print("Test failed:" + line)
    else:
        print("Test success")

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-t', '--test', default=False,
                        help='Run self tests', action='store_true')
    parser.add_argument('-F', '--fast', default=False,
                        help='Use fast mode by only checking files changed in git',
                        action='store_true')
    args = parser.parse_args()

    if args.test:
        for line, expected in tests:
            validate(line, expected)
        sys.exit(0)

    # Move up to root dir.
    os.chdir("..")

    # Some files aren't expected to comply with WiredTiger style. Ignore them.
    ignore_files = [
        'src/support/mtx_rw.c',
    ]

    command = "find bench examples ext src test -name \"*.[ch]\" \
        -o \( -name \"*.in\" ! -name \"Makefile.in\" \) \
        -o -name \"*.cpp\" -o -name \"*.i\" "
    if args.fast:
        command = "git diff --name-only $(git merge-base --fork-point develop) bench \
            examples ext src test | grep -E '(.c|.h|.cpp|.in|.i)$'"

    result = subprocess.run(command, shell=True, capture_output=True, text=True).stdout.strip('\n')
    count = 0
    if result:
        for file_name in result.split('\n'):
            if file_name in ignore_files:
                continue

            if file_is_cpp(file_name):
                count += check_cpp_comments(file_name)
            else:
                count += check_c_comments(file_name)

    if (count != 0):
        print('Detected ' + str(count) +' comment format issues!')
        sys.exit(1)

