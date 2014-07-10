#!/usr/bin/env python

"""Produces a report of all assertions in the MongoDB server codebase.

Parses .cpp files for assertions and verifies assertion codes are distinct.
Optionally replaces zero codes in source code with new distinct values.
"""

import os
import re
import utils
from collections import defaultdict, namedtuple
from optparse import OptionParser

ASSERT_NAMES = [ "uassert" , "massert", "fassert", "fassertFailed" ]
MINIMUM_CODE = 10000

codes = []

# Each AssertLocation identifies the C++ source location of an assertion
AssertLocation = namedtuple( "AssertLocation", ['sourceFile', 'lineNum', 'lines', 'code'] )


# Of historical interest only
def assignErrorCodes():
    cur = MINIMUM_CODE
    for root in ASSERT_NAMES:
        for x in utils.getAllSourceFiles():
            print( x )
            didAnything = False
            fixed = ""
            for line in open( x ):
                s = line.partition( root + "(" )
                if s[1] == "" or line.startswith( "#define " + root):
                    fixed += line
                    continue
                fixed += s[0] + root + "( " + str( cur ) + " , " + s[2]
                cur = cur + 1
                didAnything = True
            if didAnything:
                out = open( x , 'w' )
                out.write( fixed )
                out.close()


def parseSourceFiles( callback ):
    """Walks MongoDB sourcefiles and invokes callback for each AssertLocation found."""

    quick = [ "assert" , "Exception"]

    patterns = [
        re.compile( r"[umsgf]asser(?:t|ted) *\( *(\d+)" ) ,
        re.compile( r"(?:User|Msg|MsgAssertion)Exception *\( *(\d+)" ),
        re.compile( r"fassertFailed(?:NoTrace|WithStatus)? *\( *(\d+)" )
    ]

    bad = [ re.compile( r"^\s*assert *\(" ) ]

    for sourceFile in utils.getAllSourceFiles():
        if not sourceFile.find( "src/mongo/" ) >= 0:
            # Skip generated sources
            continue

        with open(sourceFile) as f:
            line_iterator = enumerate(f, 1)
            for (lineNum, line) in line_iterator:

                # See if we can skip regexes
                if not any([zz in line for zz in quick]):
                    continue

                for b in bad:
                    if b.search(line):
                        print( "%s\n%d" % (sourceFile, line) )
                        msg = "Bare assert prohibited. Replace with [umwdf]assert"
                        raise Exception(msg)

                # no more than one pattern should ever match
                matches = [x for x in [p.search(line) for p in patterns]
                           if x]
                assert len(matches) <= 1, matches
                if matches:
                    match = matches[0]
                    code = match.group(1)
                    span = match.span()

                    # Advance to statement terminator iff not on this line
                    lines = [line.strip()]

                    if not isTerminated(lines):
                        for (_lineNum, line) in line_iterator:
                            lines.append(line.strip())
                            if isTerminated(lines):
                                break

                    thisLoc = AssertLocation(sourceFile, lineNum, lines, code)
                    callback( thisLoc )

        # end for sourceFile loop


def isTerminated( lines ):
    """Given .cpp/.h source lines as text, determine if assert is terminated."""
    x = " ".join(lines)
    return ';' in x \
        or x.count('(') - x.count(')') <= 0


def getNextCode():
    """Finds next unused assertion code.

    Called by: SConstruct and main()
    Since SConstruct calls us, codes[] must be global OR WE REPARSE EVERYTHING
    """
    if not len(codes) > 0:
        readErrorCodes()

    highest = reduce( lambda x, y: max(int(x), int(y)),
                      (loc.code for loc in codes) )
    return highest + 1


def checkErrorCodes():
    """SConstruct expects a boolean response from this function.
    """
    (codes, errors) = readErrorCodes()
    return len( errors ) == 0


def readErrorCodes():
    """Defines callback, calls parseSourceFiles() with callback,
    and saves matches to global codes list.
    """
    seen = {}
    errors = []
    dups = defaultdict(list)

    # define callback
    def checkDups( assertLoc ):
        codes.append( assertLoc )
        code = assertLoc.code

        if not code in seen:
            seen[code] = assertLoc
        else:
            if not code in dups:
                # on first duplicate, add original to dups, errors
                dups[code].append( seen[code] )
                errors.append( seen[code] )

            dups[code].append( assertLoc )
            errors.append( assertLoc )

    parseSourceFiles( checkDups )

    if seen.has_key("0"):
        code = "0"
        bad = seen[code]
        errors.append( bad )
        print( "ZERO_CODE:" )
        print( "  %s:%d:%s" % (bad.sourceFile, bad.lineNum, bad.lines[0]) )

    for code, locations in dups.items():
        print( "DUPLICATE IDS: %s" % code )
        for loc in locations:
            print( "  %s:%d:%s" % (loc.sourceFile, loc.lineNum, loc.lines[0]) )

    return (codes, errors)


def replaceBadCodes( errors, nextCode ):
    """Modifies C++ source files to replace invalid assertion codes.
    For now, we only modify zero codes.

    Args:
        errors: list of AssertLocation
        nextCode: int, next non-conflicting assertion code
    """
    zero_errors = [e for e in errors if int(e.code) == 0]
    skip_errors = [e for e in errors if int(e.code) != 0]

    for loc in skip_errors:
        print "SKIPPING NONZERO code=%s: %s:%s" % (loc.code, loc.sourceFile, loc.lineNum)

    for assertLoc in zero_errors:
        (sourceFile, lineNum, lines, code) = assertLoc
        print "UPDATING_FILE: %s:%s" % (sourceFile, lineNum)

        ln = lineNum - 1

        with open(sourceFile, 'r+') as f:
            fileLines = f.readlines()
            line = fileLines[ln]
            print "LINE_%d_BEFORE:%s" % (lineNum, line.rstrip())
            line = re.sub(r'(\( *)(\d+)',
                          r'\g<1>' + str(nextCode),
                          line)
            print "LINE_%d_AFTER :%s" % (lineNum, line.rstrip())
            fileLines[ln] = line
            f.seek(0)
            f.writelines(fileLines)
        nextCode += 1


def getBestMessage( lines , codeStr ):
    """Extracts message from one AssertionLocation.lines entry

    Args:
        lines: list of contiguous C++ source lines
        codeStr: assertion code found in first line
    """
    line = lines if isinstance(lines, str) else " ".join(lines)

    err = line.partition( codeStr )[2]
    if not err:
        return ""

    # Trim to outer quotes
    m = re.search(r'"(.*)"', err)
    if not m:
        return ""
    err = m.group(1)

    # Trim inner quote pairs
    err = re.sub(r'" +"', '', err)
    err = re.sub(r'" *<< *"', '', err)
    err = re.sub(r'" *<<[^<]+<< *"', '<X>', err)
    err = re.sub(r'" *\+[^+]+\+ *"', '<X>', err)

    # Trim escaped quotes
    err = re.sub(r'\\"', '', err)

    # Iff doublequote still present, trim that and any trailing text
    err = re.sub(r'".*$', '', err)

    return err.strip()


def writeMarkdownReport( codes, outfile ):
    """Write errors.md report to filesystem in Markdown format

    Args:
        codes: list of AssertLocation
        outfile: string path to a file to overwrite
    """

    baseurl = "http://github.com/mongodb/mongo/blob/master"

    if os.path.exists(outfile):
        i = open(outfile, "r" )
        i.close()

    out = open(outfile, 'wb')
    out.write("MongoDB Error Codes\n")
    out.write("===================\n\n")
    out.write("This file is generated by errorcodes.py. Do not edit.\n\n")

    prev = ""
    seen = {}

    # Sort by sourceFile, then code
    codes.sort( key=lambda loc: loc.sourceFile+"-"+loc.code )

    for assertLoc in codes:
        if assertLoc.code in seen:
            continue
        seen[assertLoc.code] = True

        (sourceFile, lineNum, lines, code) = assertLoc

        if sourceFile.startswith("./"):
            sourceFile = sourceFile[2:]

        if sourceFile != prev:
            out.write("\n\n%s\n----\n" % sourceFile)
            prev = sourceFile

        url = "%s/%s#L%s" % (baseurl, sourceFile, lineNum)
        message = getBestMessage(lines, str(code))
        out.write("* %s [code](%s) %s\n" % (code, url, message))

    out.write( "\n" )
    out.close()


def main():
    parser = OptionParser(description=__doc__.strip())
    parser.add_option("--fix", dest="replace",
                      action="store_true", default=False,
                      help="Fix zero codes in source files [default: %default]")
    parser.add_option("-o", dest="outfile",
                      default="docs/errors.md",
                      help="Report file [default: %default]")
    (options, args) = parser.parse_args()

    (codes, errors) = readErrorCodes()
    ok = len(errors) == 0
    next = getNextCode()

    print("ok: %s" % ok)
    print("next: %s" % next)

    if ok:
        writeMarkdownReport(codes, options.outfile)
    elif options.replace:
        replaceBadCodes(errors, next)
    else:
        print ERROR_HELP


ERROR_HELP = """
ERRORS DETECTED. To correct, run "buildscripts/errorcodes.py --fix" to replace zero codes.
Other errors require manual correction.
"""

if __name__ == "__main__":
    main()
