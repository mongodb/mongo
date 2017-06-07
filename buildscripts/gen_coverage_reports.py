import os
import sys
import subprocess
from optparse import OptionParser

""" takes a list of info files or index file if info file, generate something for that 
if index file, read through all info files listed in index file and generate html for that"""

mongo_repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
html_dir = os.path.join(os.getcwd(), 'build', 'coverage', 'html')
tests = []

def processList(listname):
    """Read through a file listing tracefiles line by line.
    Call genHTML for each tracefile listed."""
    f = open(listname)
    
    for line in f:
        rc = genHTML(line.strip())
        if rc != 0:
            return rc
    
    return 0

def genIndex():
    f = open(os.path.join(html_dir, 'index.html'), 'w')
    
    f.write('<html>\n<head><title>Coverage Reports</title></head>\n<body>\n')

    for name in tests:
        f.write('<p><a href="%(name)s/index.html">%(name)s</a></p>\n' % {'name': name})

    f.write('</body></html>')


def genHTML(path):
    """Take the path to a tracefile and generate coverage report html for it.
    The html is placed in a subdirectory of ./build/coverage/html"""
    if os.path.isfile(path) and os.path.getsize(path) > 0:
        print "Processing " + path
        devnull = open(os.devnull, 'w')
        bname = os.path.basename(path)
        name, ext= os.path.splitext(bname)
        tests.append(name)
        return subprocess.call(['genhtml', path, '-o', os.path.join(html_dir, name)],
                               stdout = devnull, stderr = devnull)    
    return 0
        
def main():
    parser = OptionParser(usage = "%prog input.info [input.txt ...]")

    (options, args) = parser.parse_args()

    for fullname in args:
        bname = os.path.basename(fullname)
        name,ext = os.path.splitext(bname)
        if ext == ".txt":
            rc = processList(fullname)
        elif ext == ".info":
            rc = genHTML(fullname)
        else:
            return "Unrecognized file type " + ext
        
        if rc != 0:
            return "Failed to process " + fullname

    genIndex()

if __name__ == "__main__":
    sys.exit(main())
