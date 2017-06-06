#!/usr/bin/env python
#
# Public Domain 2014-2017 MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# runner/core.py
#   Core functions available to all runners
import glob, os
import workgen

# txn --
#   Put the operation (and any suboperations) within a transaction.
def txn(op, config=None):
    t = workgen.Transaction(config)
    op._transaction = t
    return op

# Check for a local build that contains the wt utility. First check in
# current working directory, then in build_posix and finally in the disttop
# directory. This isn't ideal - if a user has multiple builds in a tree we
# could pick the wrong one.
def _wiredtiger_builddir():
    if os.path.isfile(os.path.join(os.getcwd(), 'wt')):
        return os.getcwd()

    # The directory of this file should be within the distribution tree.
    thisdir = os.path.dirname(os.path.abspath(__file__))
    wt_disttop = os.path.join(\
        thisdir, os.pardir, os.pardir, os.pardir, os.pardir)
    if os.path.isfile(os.path.join(wt_disttop, 'wt')):
        return wt_disttop
    if os.path.isfile(os.path.join(wt_disttop, 'build_posix', 'wt')):
        return os.path.join(wt_disttop, 'build_posix')
    if os.path.isfile(os.path.join(wt_disttop, 'wt.exe')):
        return wt_disttop
    raise Exception('Unable to find useable WiredTiger build')

# Return the wiredtiger_open extension argument for any needed shared library.
# Called with a list of extensions, e.g.
#    [ 'compressors/snappy', 'encryptors/rotn=config_string' ]
def extensions_config(exts):
    result = ''
    extfiles = {}
    errpfx = 'extensions_config'
    builddir = _wiredtiger_builddir()
    for ext in exts:
        extconf = ''
        if '=' in ext:
            splits = ext.split('=', 1)
            ext = splits[0]
            extconf = '=' + splits[1]
        splits = ext.split('/')
        if len(splits) != 2:
            raise Exception(errpfx + ": " + ext +
                ": extension is not named <dir>/<name>")
        libname = splits[1]
        dirname = splits[0]
        pat = os.path.join(builddir, 'ext',
            dirname, libname, '.libs', 'libwiredtiger_*.so')
        filenames = glob.glob(pat)
        if len(filenames) == 0:
            raise Exception(errpfx +
                ": " + ext +
                ": no extensions library found matching: " + pat)
        elif len(filenames) > 1:
            raise Exception(errpfx + ": " + ext +
                ": multiple extensions libraries found matching: " + pat)
        complete = '"' + filenames[0] + '"' + extconf
        if ext in extfiles:
            if extfiles[ext] != complete:
                raise Exception(errpfx +
                    ": non-matching extension arguments in " +
                    str(exts))
        else:
            extfiles[ext] = complete
    if len(extfiles) != 0:
        result = ',extensions=[' + ','.join(extfiles.values()) + ']'
    return result
