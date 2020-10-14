from __future__ import print_function
import filecmp, fnmatch, glob, os, re, shutil, subprocess

# source_files --
#    Return a list of the WiredTiger source file names.
def source_files():
    file_re = re.compile(r'^\w')
    for line in glob.iglob('../src/include/*.h'):
        yield line
    for line in open('filelist', 'r'):
        if file_re.match(line):
            yield os.path.join('..', line.split()[0])
    for line in open('extlist', 'r'):
        if file_re.match(line):
            yield os.path.join('..', line.split()[0])

# all_c_files --
#       Return list of all WiredTiger C source file names.
def all_c_files():
    file_re = re.compile(r'^\w')
    for line in glob.iglob('../src/*/*.c'):
        yield line
    for line in glob.iglob('../src/*/*_inline.h'):
        yield line
    files = list()
    for (dirpath, dirnames, filenames) in os.walk('../test'):
        files += [os.path.join(dirpath, file) for file in filenames]
    for file in files:
        if fnmatch.fnmatch(file, '*.c'):
            yield file
        if fnmatch.fnmatch(file, '*_inline.h'):
            yield file

# all_h_files --
#       Return list of all WiredTiger C include file names.
def all_h_files():
    file_re = re.compile(r'^\w')
    for line in glob.iglob('../src/*/*.h'):
        yield line
    yield "../src/include/wiredtiger.in"
    files = list()
    for (dirpath, dirnames, filenames) in os.walk('../test'):
        files += [os.path.join(dirpath, file) for file in filenames]
    for file in files:
        if fnmatch.fnmatch(file, '*.h'):
            yield file

# source_dirs --
#    Return a list of the WiredTiger source directory names.
def source_dirs():
    dirs = set()
    for f in source_files():
        dirs.add(os.path.dirname(f))
    return dirs

def print_source_dirs():
    for d in source_dirs():
        print(d)

# compare_srcfile --
#    Compare two files, and if they differ, update the source file.
def compare_srcfile(tmp, src):
    if not os.path.isfile(src) or not filecmp.cmp(tmp, src, shallow=False):
        print(('Updating ' + src))
        shutil.copyfile(tmp, src)
    os.remove(tmp)

# format_srcfile --
#    Format a source file.
def format_srcfile(src):
    src = os.path.abspath(src)
    subprocess.check_call(['./s_clang-format', src])
