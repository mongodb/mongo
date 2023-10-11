#!/usr/bin/env python3

import filecmp, fnmatch, glob, os, re, shutil, subprocess
from contextlib import contextmanager

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
    try:
        subprocess.check_call(['./s_clang_format', src])
    except subprocess.CalledProcessError as e:
        print(e)

# ModifyFile --
#    This manages a file that may be modified, possibly multiple times.
# and at the end, may need to be formatted and compared to the original.
# All modifications must be done using replace_fragment.
class ModifyFile:
    def __init__(self, filename):
        self.final_name = filename
        self.mod_name = filename + ".MOD"
        self.tmp_name = filename + ".TMP"
        self.current = filename

    # Remove a possibly nonexistent file
    def remove(self, filename):
        try:
            os.remove(filename)
        except:
            pass

    @contextmanager
    def replace_fragment(self, match):
        tfile = open(self.tmp_name, 'w')
        skip = False
        try:
            for line in open(self.current, 'r'):
                if skip:
                    if match + ': END' in line:
                        tfile.write('/*\n' + line)
                        skip = False
                else:
                    tfile.write(line)
                if match + ': BEGIN' in line:
                    skip = True
                    tfile.write(' */\n')
                    yield tfile
        finally:
            tfile.close()

        self.remove(self.mod_name)
        os.rename(self.tmp_name, self.mod_name)
        self.current = self.mod_name

    # Called to signal we are done with all modifications.
    # The modified file should be formatted and compared against
    # the original, potentially moving a new version into place.
    def done(self, format=True):
        if self.current == self.final_name:
            # Nothing was changed
            return
        if format:
            format_srcfile(self.mod_name)
        compare_srcfile(self.mod_name, self.final_name)
        self.remove(self.tmp_name)
