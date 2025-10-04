# Copyright Deniz Bahadir 2015
#
# Distributed under the Boost Software License, Version 1.0. 
# (See accompanying file LICENSE_1_0.txt or copy at 
# http://www.boost.org/LICENSE_1_0.txt)
#
# See http://www.boost.org/libs/mpl for documentation.
# See http://stackoverflow.com/a/29627158/3115457 for further information.

import argparse
import sys
import os.path
import re
import fileinput
import datetime
import glob


def check_header_comment(filename):
    """Checks if the header-comment of the given file needs fixing."""
    # Check input file.
    name = os.path.basename( filename )
    # Read content of input file.
    sourcefile = open( filename, "rU" )
    content = sourcefile.read()
    sourcefile.close()
    # Search content for '$Id$'.
    match = re.search(r'\$Id\$', content)
    if match == None:
        # Make sure that the correct value for '$Id$' was already set.
        match = re.search(r'\$Id: ' + name + r'\s+[^$]+\$', content)
        if match != None:
            # The given file needs no fixing.
            return False
    # The given file needs fixing.
    return True


def check_input_files_for_variadic_seq(headerDir, sourceDir):
    """Checks if files, used as input when pre-processing MPL-containers in their variadic form, need fixing."""
    # Check input files in include/source-directories.
    files  = glob.glob( os.path.join( headerDir, "*.hpp" ) )
    files += glob.glob( os.path.join( headerDir, "aux_", "*.hpp" ) )
    files += glob.glob( os.path.join( sourceDir, "src", "*" ) )
    for currentFile in sorted( files ):
        if check_header_comment( currentFile ):
            return True
    return False


def check_input_files_for_numbered_seq(sourceDir, suffix, containers):
    """Check if files, used as input when pre-processing MPL-containers in their numbered form, need fixing."""
    # Check input files for each MPL-container type.
    for container in containers:
        files = glob.glob( os.path.join( sourceDir, container, container + '*' + suffix ) )
        for currentFile in sorted( files ):
            if check_header_comment( currentFile ):
                return True
    return False


def check_input_files(headerDir, sourceDir, containers=['vector', 'list', 'set', 'map'],
                      seqType='both', verbose=False):
    """Checks if source- and header-files, used as input when pre-processing MPL-containers, need fixing."""
    # Check the input files for containers in their variadic form.
    result1 = False
    if seqType == "both" or seqType == "variadic":
        if verbose:
            print "Check if input files for pre-processing Boost.MPL variadic containers need fixing."
        result1 = check_input_files_for_variadic_seq(headerDir, sourceDir)
        if verbose:
            if result1:
                print "  At least one input file needs fixing!"
            else:
                print "  No input file needs fixing!"
    # Check the input files for containers in their numbered form.
    result2 = False
    result3 = False
    if seqType == "both" or seqType == "numbered":
        if verbose:
            print "Check input files for pre-processing Boost.MPL numbered containers."
        result2 = check_input_files_for_numbered_seq(headerDir, ".hpp", containers)
        result3 = check_input_files_for_numbered_seq(sourceDir, ".cpp", containers)
        if verbose:
            if result2 or result3:
                print "  At least one input file needs fixing!"
            else:
                print "  No input file needs fixing!"
    # Return result.
    return result1 or result2 or result3

def fix_header_comment(filename, timestamp):
    """Fixes the header-comment of the given file."""
    # Fix input file.
    name = os.path.basename( filename )
    for line in fileinput.input( filename, inplace=1, mode="rU" ):
        # If header-comment already contains anything for '$Id$', remove it.
        line = re.sub(r'\$Id:[^$]+\$', r'$Id$', line.rstrip())
        # Replace '$Id$' by a string containing the file's name (and a timestamp)!
        line = re.sub(re.escape(r'$Id$'), r'$Id: ' + name + r' ' + timestamp.isoformat() + r' $', line.rstrip())
        print(line)


def fix_input_files_for_variadic_seq(headerDir, sourceDir, timestamp):
    """Fixes files used as input when pre-processing MPL-containers in their variadic form."""
    # Fix files in include/source-directories.
    files  = glob.glob( os.path.join( headerDir, "*.hpp" ) )
    files += glob.glob( os.path.join( headerDir, "aux_", "*.hpp" ) )
    files += glob.glob( os.path.join( sourceDir, "src", "*" ) )
    for currentFile in sorted( files ):
        fix_header_comment( currentFile, timestamp )


def fix_input_files_for_numbered_seq(sourceDir, suffix, timestamp, containers):
    """Fixes files used as input when pre-processing MPL-containers in their numbered form."""
    # Fix input files for each MPL-container type.
    for container in containers:
        files = glob.glob( os.path.join( sourceDir, container, container + '*' + suffix ) )
        for currentFile in sorted( files ):
            fix_header_comment( currentFile, timestamp )


def fix_input_files(headerDir, sourceDir, containers=['vector', 'list', 'set', 'map'],
                    seqType='both', verbose=False):
    """Fixes source- and header-files used as input when pre-processing MPL-containers."""
    # The new modification time.
    timestamp = datetime.datetime.now();
    # Fix the input files for containers in their variadic form.
    if seqType == "both" or seqType == "variadic":
        if verbose:
            print "Fix input files for pre-processing Boost.MPL variadic containers."
        fix_input_files_for_variadic_seq(headerDir, sourceDir, timestamp)
    # Fix the input files for containers in their numbered form.
    if seqType == "both" or seqType == "numbered":
        if verbose:
            print "Fix input files for pre-processing Boost.MPL numbered containers."
        fix_input_files_for_numbered_seq(headerDir, ".hpp", timestamp, containers)
        fix_input_files_for_numbered_seq(sourceDir, ".cpp", timestamp, containers)


def to_existing_absolute_path(string):
    """Converts a path into its absolute path and verifies that it exists or throws an exception."""
    value = os.path.abspath(string)
    if not os.path.exists( value ) or not os.path.isdir( value ):
        msg = '"%r" is not a valid path to a directory.' % string
        raise argparse.ArgumentTypeError(msg)
    return value


def main():
    """The main function."""

    # Prepare and run cmdline-parser.
    cmdlineParser = argparse.ArgumentParser(
                    description="Fixes the input files used for pre-processing of Boost.MPL headers.")
    cmdlineParser.add_argument("-v", "--verbose", dest='verbose', action='store_true',
                               help="Be a little bit more verbose.")
    cmdlineParser.add_argument("--check-only", dest='checkonly', action='store_true',
                               help="Only checks if fixing is required.")
    cmdlineParser.add_argument(dest='sourceDir', metavar="<source-dir>",
                               type=to_existing_absolute_path,
                               help="The source-directory of Boost.")
    args = cmdlineParser.parse_args()

    # Some verbose debug output.
    if args.verbose:
        print "Arguments extracted from command-line:"
        print "  verbose           = ", args.verbose
        print "  check-only        = ", args.checkonly
        print "  source directory  = ", args.sourceDir

    # The directories for header- and source files of Boost.MPL.    
    # NOTE: Assuming 'args.sourceDir' is the source-directory of the entire boost project.
    headerDir = os.path.join( args.sourceDir, "boost", "mpl" )
    sourceDir = os.path.join( args.sourceDir, "libs", "mpl", "preprocessed" )
    # Check that the header/source-directories exist.
    if not os.path.exists( headerDir ) or not os.path.exists( sourceDir ):
        # Maybe 'args.sourceDir' is not the source-directory of the entire boost project
        # but instead of the Boost.MPL git-directory, only?
        headerDir = os.path.join( args.sourceDir, "include", "boost", "mpl" )
        sourceDir = os.path.join( args.sourceDir, "preprocessed" )
        if not os.path.exists( headerDir ) or not os.path.exists( sourceDir ):
            cmdlineParser.print_usage()
            print "error: Cannot find Boost.MPL header/source files in given Boost source-directory!"
            sys.exit(0)

    # Some verbose debug output.
    if args.verbose:
        print "Chosen header-directory: ", headerDir
        print "Chosen source-directory: ", sourceDir

    if args.checkonly:
        # Check input files for generating pre-processed headers.
        result = check_input_files(headerDir, sourceDir, verbose = args.verbose)
        if result:
            print "Fixing the input-files used for pre-processing of Boost.MPL headers IS required."
        else:
            print "Fixing the input-files used for pre-processing of Boost.MPL headers is NOT required."
    else:
        # Fix input files for generating pre-processed headers.
        fix_input_files(headerDir, sourceDir, verbose = args.verbose)


if __name__ == '__main__':
    main()
