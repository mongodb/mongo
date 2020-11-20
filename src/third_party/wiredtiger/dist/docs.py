# Read and verify the documentation data to make sure path names are valid.

import os, sys
import docs_data

def check_sort(got, msg, keyfunc=None):
    if keyfunc:
        expect = sorted(got, key=keyfunc)
    else:
        expect = sorted(got)
    if got != expect:
        print(msg)
        print('  got: ' + str(got))
        print('  expect: ' + str(expect))

# An include filename will be sorted first.
def inc_first(f):
    if '/include/' in f:
        return '_' + f
    else:
        return f

top_dir = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))

pages = docs_data.arch_doc_pages
all_names = [ page.doxygen_name for page in pages]
check_sort(all_names, 'arch_doc_pages must be sorted by name.')

for page in pages:
    name = page.doxygen_name
    check_sort(page.data_structures, name + ': data structures must be sorted.')
    for partial in page.files:
        fullpath = os.path.join(top_dir, partial)
        if not os.path.exists(fullpath):
            print(name + ': ' + partial + ': does not exist')
        elif os.path.isdir(fullpath):
            if fullpath[-1:] != '/':
                print(name + ': ' + partial + ': is a directory, must end in /')
        else:
            if fullpath[-1:] == '/':
                print(name + ': ' + partial + ': not a directory, cannot end in /')
    check_sort(page.files,
      name + ': sources must be sorted, with include files first.', inc_first)

sys.exit(0)
