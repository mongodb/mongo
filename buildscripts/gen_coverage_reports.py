import os
import sys
import subprocess

""" takes a list of info files or index file if info file, generate something for that 
if index file, read through all info files listed in index file and generate html for that"""

html_dir = os.getcwd() + '/build/coverage/html'

def processList(listname):
    """Read through a file listing tracefiles line by line.
    Call genHTML for each tracefile listed."""
    f = open(listname)
    for line in f:
        genHTML(line.strip())
        
def genHTML(path):
    """Take the path to a tracefile and generate coverage report html for it.
    The html is placed in a subdirectory of ./build/coverage/html"""
    if os.path.getsize(path) > 0:
        bname = os.path.basename(path)
        name,ext= os.path.splitext(bname)
        subprocess.call(['genhtml', path, '-o', os.path.join(html_dir, name)])    
        
def main(argv):
    for fullname in argv[1:]:
        bname = os.path.basename(fullname)
        name,ext = os.path.splitext(bname)
        if ext == ".txt":
            processList(fullname)
        if ext == ".info":
            genHTML(fullname)

if __name__ == "__main__":
    rc = main(sys.argv) or 0
    sys.exit(rc)
        
