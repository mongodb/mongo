"""SCons.Tool.zip

Tool-specific initialization for zip.

There normally shouldn't be any need to import this module directly.
It will usually be imported through the generic SCons.Tool.Tool()
selection method.

"""

# MIT License
#
# Copyright The SCons Foundation
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

import os

import SCons.Builder
import SCons.Defaults
import SCons.Node.FS
import SCons.Util

import time
import zipfile


zip_compression = zipfile.ZIP_DEFLATED


def _create_zipinfo_for_file(fname, arcname, date_time, compression):
    st = os.stat(fname)
    if not date_time:
        mtime = time.localtime(st.st_mtime)
        date_time = mtime[0:6]
    zinfo = zipfile.ZipInfo(filename=arcname, date_time=date_time)
    zinfo.external_attr = (st.st_mode & 0xFFFF) << 16  # Unix attributes
    zinfo.compress_type = compression
    zinfo.file_size = st.st_size
    return zinfo


def zip_builder(target, source, env) -> None:
    compression = env.get('ZIPCOMPRESSION', zipfile.ZIP_STORED)
    zip_root = str(env.get('ZIPROOT', ''))
    date_time = env.get('ZIP_OVERRIDE_TIMESTAMP')

    files = []
    for s in source:
        if s.isdir():
            for dirpath, dirnames, filenames in os.walk(str(s)):
                for fname in filenames:
                    path = os.path.join(dirpath, fname)
                    if os.path.isfile(path):
                        files.append(path)
        else:
            files.append(str(s))

    with zipfile.ZipFile(str(target[0]), 'w', compression) as zf:
        for fname in files:
            arcname = os.path.relpath(fname, zip_root)
            # TODO: Switch to ZipInfo.from_file when 3.6 becomes the base python version
            zinfo = _create_zipinfo_for_file(fname, arcname, date_time, compression)
            with open(fname, "rb") as f:
                zf.writestr(zinfo, f.read())


# Fix PR #3569 - If you don't specify ZIPCOM and ZIPCOMSTR when creating
# env, then it will ignore ZIPCOMSTR set afterwards.
zipAction = SCons.Action.Action(zip_builder, "$ZIPCOMSTR",
                                varlist=['ZIPCOMPRESSION', 'ZIPROOT', 'ZIP_OVERRIDE_TIMESTAMP'])

ZipBuilder = SCons.Builder.Builder(action=SCons.Action.Action('$ZIPCOM', '$ZIPCOMSTR'),
                                   source_factory=SCons.Node.FS.Entry,
                                   source_scanner=SCons.Defaults.DirScanner,
                                   suffix='$ZIPSUFFIX',
                                   multi=1)


def generate(env) -> None:
    """Add Builders and construction variables for zip to an Environment."""
    try:
        bld = env['BUILDERS']['Zip']
    except KeyError:
        bld = ZipBuilder
        env['BUILDERS']['Zip'] = bld

    env['ZIP'] = 'zip'
    env['ZIPFLAGS'] = SCons.Util.CLVar('')
    env['ZIPCOM'] = zipAction
    env['ZIPCOMPRESSION'] = zip_compression
    env['ZIPSUFFIX'] = '.zip'
    env['ZIPROOT'] = SCons.Util.CLVar('')


def exists(env) -> bool:
    return True

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
