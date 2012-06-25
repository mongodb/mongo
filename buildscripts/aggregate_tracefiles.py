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

    print ' '.join(args)
    
    return subprocess.call(args)  

def getfilesize(path):
    if not os.path.isfile(path):
        return 0
    return os.path.getsize(path)

def main ():
    inputs = []

    usage = "usage: %prog input1.info input2.info ... output.info" 
    parser = OptionParser(usage=usage)

    (options, args) = parser.parse_args()
    if len(args) < 2:
        return "must supply input files"

    for path in args[:-1]:
        name, ext = os.path.splitext(path)

        if ext == '.info':
            if getfilesize(path) > 0:
                inputs.append(path)

        elif ext == '.txt':
            inputs += [line.strip() for line in open(path) 
                        if getfilesize(line.strip()) > 0]
        else:
            return "unrecognized file type"

    return aggregate(inputs, args[-1])

if __name__ == '__main__':
    sys.exit(main())
