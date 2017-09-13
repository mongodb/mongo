import filecmp, glob, os, re, shutil

# source_files --
#    Return a list of the WiredTiger source file names.
def source_files():
    file_re = re.compile(r'^\w')
    for line in glob.iglob('../src/include/*.[hi]'):
        yield line
    for line in open('filelist', 'r'):
        if file_re.match(line):
            yield os.path.join('..', line.split()[0])
    for line in open('extlist', 'r'):
        if file_re.match(line):
            yield os.path.join('..', line.split()[0])

# source_dirs --
#    Return a list of the WiredTiger source directory names.
def source_dirs():
    dirs = set()
    for f in source_files():
        dirs.add(os.path.dirname(f))
    return dirs

def print_source_dirs():
    for d in source_dirs():
        print d

# compare_srcfile --
#    Compare two files, and if they differ, update the source file.
def compare_srcfile(tmp, src):
    if not os.path.isfile(src) or not filecmp.cmp(tmp, src, shallow=False):
        print('Updating ' + src)
        shutil.copyfile(tmp, src)
    os.remove(tmp)
