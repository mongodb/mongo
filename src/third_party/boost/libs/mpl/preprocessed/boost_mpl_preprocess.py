# Copyright Deniz Bahadir 2015
#
# Distributed under the Boost Software License, Version 1.0. 
# (See accompanying file LICENSE_1_0.txt or copy at 
# http://www.boost.org/LICENSE_1_0.txt)
#
# See http://www.boost.org/libs/mpl for documentation.
# See http://stackoverflow.com/a/20660264/3115457 for further information.
# See http://stackoverflow.com/a/29627158/3115457 for further information.

import fix_boost_mpl_preprocess as fixmpl
import argparse
import sys
import os
import os.path
import re
import fileinput
import shutil


def create_more_container_files(sourceDir, suffix, maxElements, containers, containers2):
    """Creates additional files for the individual MPL-containers."""

    # Create files for each MPL-container with 20 to 'maxElements' elements
    # which will be used during generation.
    for container in containers:
        for i in range(20, maxElements, 10):
            # Create copy of "template"-file.
            newFile = os.path.join( sourceDir, container, container + str(i+10) + suffix )
            shutil.copyfile( os.path.join( sourceDir, container, container + "20" + suffix ), newFile ) 
            # Adjust copy of "template"-file accordingly.
            for line in fileinput.input( newFile, inplace=1, mode="rU" ):
                line = re.sub(r'20', '%TWENTY%', line.rstrip())
                line = re.sub(r'11', '%ELEVEN%', line.rstrip())
                line = re.sub(r'10(?![0-9])', '%TEN%', line.rstrip())
                line = re.sub(r'%TWENTY%', re.escape(str(i+10)), line.rstrip())
                line = re.sub(r'%ELEVEN%', re.escape(str(i + 1)), line.rstrip())
                line = re.sub(r'%TEN%', re.escape(str(i)), line.rstrip())
                print(line)
    for container in containers2:
        for i in range(20, maxElements, 10):
            # Create copy of "template"-file.
            newFile = os.path.join( sourceDir, container, container + str(i+10) + "_c" + suffix )
            shutil.copyfile( os.path.join( sourceDir, container, container + "20_c" + suffix ), newFile ) 
            # Adjust copy of "template"-file accordingly.
            for line in fileinput.input( newFile, inplace=1, mode="rU" ):
                line = re.sub(r'20', '%TWENTY%', line.rstrip())
                line = re.sub(r'11', '%ELEVEN%', line.rstrip())
                line = re.sub(r'10(?![0-9])', '%TEN%', line.rstrip())
                line = re.sub(r'%TWENTY%', re.escape(str(i+10)), line.rstrip())
                line = re.sub(r'%ELEVEN%', re.escape(str(i + 1)), line.rstrip())
                line = re.sub(r'%TEN%', re.escape(str(i)), line.rstrip())
                print(line)


def create_input_for_numbered_sequences(headerDir, sourceDir, containers, maxElements):
    """Creates additional source- and header-files for the numbered sequence MPL-containers."""
    # Create additional container-list without "map".
    containersWithoutMap = containers[:]
    try:
        containersWithoutMap.remove('map')
    except ValueError:
        # We can safely ignore if "map" is not contained in 'containers'!
        pass
    # Create header/source-files.
    create_more_container_files(headerDir, ".hpp", maxElements, containers, containersWithoutMap)
    create_more_container_files(sourceDir, ".cpp", maxElements, containers, containersWithoutMap)


def adjust_container_limits_for_variadic_sequences(headerDir, containers, maxElements):
    """Adjusts the limits of variadic sequence MPL-containers."""
    for container in containers:
        headerFile = os.path.join( headerDir, "limits", container + ".hpp" )
        regexMatch   = r'(define\s+BOOST_MPL_LIMIT_' + container.upper() + r'_SIZE\s+)[0-9]+'
        regexReplace = r'\g<1>' + re.escape( str(maxElements) )
        for line in fileinput.input( headerFile, inplace=1, mode="rU" ):
            line = re.sub(regexMatch, regexReplace, line.rstrip())
            print(line)


def current_boost_dir():
    """Returns the (relative) path to the Boost source-directory this file is located in (if any)."""
    # Path to directory containing this script.
    path = os.path.dirname( os.path.realpath(__file__) )
    # Making sure it is located in "${boost-dir}/libs/mpl/preprocessed".
    for directory in reversed( ["libs", "mpl", "preprocessed"] ):
        (head, tail) = os.path.split(path)
        if tail == directory:
            path = head
        else:
            return None
    return os.path.relpath( path )
    


def to_positive_multiple_of_10(string):
    """Converts a string into its encoded positive integer (greater zero) or throws an exception."""
    try:
        value = int(string)
    except ValueError:
        msg = '"%r" is not a positive multiple of 10 (greater zero).' % string
        raise argparse.ArgumentTypeError(msg)
    if value <= 0 or value % 10 != 0:
        msg = '"%r" is not a positive multiple of 10 (greater zero).' % string
        raise argparse.ArgumentTypeError(msg)
    return value


def to_existing_absolute_path(string):
    """Converts a path into its absolute path and verifies that it exists or throws an exception."""
    value = os.path.abspath(string)
    if not os.path.exists( value ) or not os.path.isdir( value ):
        msg = '"%r" is not a valid path to a directory.' % string
        raise argparse.ArgumentTypeError(msg)
    return value


def main():
    """The main function."""
    
    # Find the current Boost source-directory in which this script is located.
    sourceDir = current_boost_dir()
    if sourceDir == None:
        sourceDir = ""
    
    # Prepare and run cmdline-parser.
    cmdlineParser = argparse.ArgumentParser(description="A generator-script for pre-processed Boost.MPL headers.")
    cmdlineParser.add_argument("-v", "--verbose", dest='verbose', action='store_true',
                               help="Be a little bit more verbose.")
    cmdlineParser.add_argument("-s", "--sequence-type", dest='seqType', choices=['variadic', 'numbered', 'both'],
                               default='both',
                               help="Only update pre-processed headers for the selected sequence types, "
                                    "either 'numbered' sequences, 'variadic' sequences or 'both' sequence "
                                    "types. (Default=both)")
    cmdlineParser.add_argument("--no-vector", dest='want_vector', action='store_false',
                               help="Do not update pre-processed headers for Boost.MPL Vector.")
    cmdlineParser.add_argument("--no-list", dest='want_list', action='store_false',
                               help="Do not update pre-processed headers for Boost.MPL List.")
    cmdlineParser.add_argument("--no-set", dest='want_set', action='store_false',
                               help="Do not update pre-processed headers for Boost.MPL Set.")
    cmdlineParser.add_argument("--no-map", dest='want_map', action='store_false',
                               help="Do not update pre-processed headers for Boost.MPL Map.")
    cmdlineParser.add_argument("--num-elements", dest='numElements', metavar="<num-elements>",
                               type=to_positive_multiple_of_10, default=100,
                               help="The maximal number of elements per container sequence. (Default=100)")
    cmdlineParser.add_argument(dest='sourceDir', metavar="<source-dir>", default=current_boost_dir(), nargs='?',
                               type=to_existing_absolute_path,
                               help="The source-directory of Boost. (Default=\"" + sourceDir + "\")")
    args = cmdlineParser.parse_args()

    # Some verbose debug output.
    if args.verbose:
        print "Arguments extracted from command-line:"
        print "  verbose          = ", args.verbose
        print "  source directory = ", args.sourceDir
        print "  num elements     = ", args.numElements
        print "  sequence type    = ", args.seqType
        print "  want: vector     = ", args.want_vector
        print "  want: list       = ", args.want_list
        print "  want: set        = ", args.want_set
        print "  want: map        = ", args.want_map

    # Verify that we received any source-directory.
    if args.sourceDir == None:
        print "You should specify a valid path to the Boost source-directory."
        sys.exit(0)

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

    # Create list of containers for which files shall be pre-processed.
    containers = []
    if args.want_vector:
        containers.append('vector')
    if args.want_list:
        containers.append('list')
    if args.want_set:
        containers.append('set')
    if args.want_map:
        containers.append('map')
    if containers == []:
        print "Nothing to do."
        print "(Why did you prevent generating pre-processed headers for all Boost.MPL container types?)"
        sys.exit(0)

    # Possibly fix the header-comments of input-files needed for pre-processing.
    if args.verbose:
        print "Checking if prior to pre-processing some input-files need fixing."
    needFixing = fixmpl.check_input_files(headerDir, sourceDir, containers, args.seqType, args.verbose)
    if needFixing:
        if args.verbose:
            print "Fixing of some input-files prior to pre-processing is needed."
            print "Will fix them now!"
        fixmpl.fix_input_files(headerDir, sourceDir, containers, args.seqType, args.verbose)

    # Some verbose debug output.
    if args.verbose:
        print "Containers for which to pre-process headers: ", containers

    # Create (additional) input files for generating pre-processed headers of numbered sequence MPL containers.
    if args.seqType == "both" or args.seqType == "numbered":
        create_input_for_numbered_sequences(headerDir, sourceDir, containers, args.numElements)
    # Modify settings for generating pre-processed headers of variadic sequence MPL containers.
    if args.seqType == "both" or args.seqType == "variadic":
        adjust_container_limits_for_variadic_sequences(headerDir, containers, args.numElements)

    # Generate MPL-preprocessed files.
    os.chdir( sourceDir )
    if args.seqType == "both" or args.seqType == "numbered":
        if args.want_vector:
            if args.verbose:
                print "Pre-process headers for Boost.MPL numbered vectors."
            os.system( "python " + os.path.join( sourceDir, "preprocess_vector.py" ) + " all " + args.sourceDir )
        if args.want_list:
            if args.verbose:
                print "Pre-process headers for Boost.MPL numbered lists."
            os.system( "python " + os.path.join( sourceDir, "preprocess_list.py" ) + " all " + args.sourceDir )
        if args.want_set:
            if args.verbose:
                print "Pre-process headers for Boost.MPL numbered sets."
            os.system( "python " + os.path.join( sourceDir, "preprocess_set.py" ) + " all " + args.sourceDir )
        if args.want_map:
            if args.verbose:
                print "Pre-process headers for Boost.MPL numbered maps."
            os.system( "python " + os.path.join( sourceDir, "preprocess_map.py" ) + " all " + args.sourceDir )
    if args.seqType == "both" or args.seqType == "variadic":
        if args.verbose:
            print "Pre-process headers for Boost.MPL variadic containers."
        os.system( "python " + os.path.join( sourceDir, "preprocess.py" ) + " all " + args.sourceDir )


if __name__ == '__main__':
    main()
