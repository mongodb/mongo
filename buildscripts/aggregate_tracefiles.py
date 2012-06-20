import subprocess
import os
import sys

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

    
    if subprocess.call(args) != 0:
        die("lcov failed")

def die(message):
    sys.stderr.write(message + "\n")
    sys.exit(1)

def main (argv):
    inputs = []

    if len(argv) < 3:
        print "Usage: " + argv[0] + " input.(info|txt) ... output.info"
        sys.exit(1)

    for path in argv[1:-1]:
        name, ext = os.path.splitext(path)

        if ext == '.info':
            if os.path.getsize(path) > 0:
                inputs.append(path)

        elif ext == '.txt':
            inputs += [line.strip() for line in open(path) 
                        if os.path.getsize(line.strip()) > 0]
        else:
            die("unrecognized file type")

    aggregate(inputs, argv[-1])

    return 0

if __name__ == '__main__':
    rc = main(sys.argv) or 0
    sys.exit(rc)
