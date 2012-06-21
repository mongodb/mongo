import subprocess
import os
import sys
from optparse import OptionParser

""" This script aggregates several tracefiles into one tracefile
 All but the last argument are input tracefiles or .txt files which list tracefiles.
 The last argument is the tracefile to which the output will be written
"""
def aggregate(inputs, output):
    """Aggregates the tracefiles given in inputs to a tracefile given by output"""
    args = ['lcov']

    for name in inputs:
        args += ['-a', name]

    args += ['-o', output]

    
    return subprocess.call(args)  


def main ():
    inputs = []

    usage = "usage: %prog [options] input1.info input2.info ..." 
    parser = OptionParser(usage=usage)
    parser.add_option('-o', '--output', dest="output",
                      help="tracefile to which output will be written")

    (options, args) = parser.parse_args()
    if len(args) == 0:
        return "must supply input files"

    for path in args:
        name, ext = os.path.splitext(path)

        if ext == '.info':
            if os.path.getsize(path) > 0:
                inputs.append(path)

        elif ext == '.txt':
            inputs += [line.strip() for line in open(path) 
                        if os.path.getsize(line.strip()) > 0]
        else:
            return "unrecognized file type"

    return aggregate(inputs, options.output)

if __name__ == '__main__':
    sys.exit(main())
