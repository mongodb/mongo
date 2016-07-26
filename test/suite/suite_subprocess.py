#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

import os, subprocess
from run import wt_builddir

# suite_subprocess.py
#    Run a subprocess within the test suite
# Used as a 'mixin' class along with a WiredTigerTestCase class
class suite_subprocess:
    subproc = None

    def has_error_in_file(self, filename):
        """
        Return whether the file contains 'ERROR'.
        WT utilities issue a 'WT_ERROR' output string upon error.
        """
        with open(filename, 'r') as f:
            for line in f:
                if 'ERROR' in line:
                    return True
        return False

    def check_no_error_in_file(self, filename, match='ERROR'):
        """
        Raise an error and show output context if the file contains 'ERROR'.
        WT utilities issue a 'WT_ERROR' output string upon error.
        """
        lines = []
        hasError = False
        hasPrevious = False  # do we need to prefix an ellipsis?
        hasNext = False  # do we need to suffix an ellipsis?
        with open(filename, 'r') as f:
            for line in f:
                lines.append(line)
                hasError = hasError or match in line
                if hasError:
                    if len(lines) > 10:
                        hasNext = True
                        break
                else:
                    if len(lines) > 5:
                        lines.pop(0)
                        hasPrevious = True
        if hasError:
            print '**************** ' + match + ' in output file: ' + filename + ' ****************'
            if hasPrevious:
                print '...'
            for line in lines:
                print line,
            if hasNext:
                print '...'
            print '********************************'
            self.fail('ERROR found in output file: ' + filename)

    def check_file_content(self, filename, expect):
        with open(filename, 'r') as f:
            got = f.read(len(expect) + 100)
            self.assertEqual(got, expect, filename + ': does not contain expected:\n\'' + expect + '\', but contains:\n\'' + got + '\'.')

    def check_file_contains(self, filename, expect):
        """
        Check that the file contains the expected string in the first 100K bytes
        """
        maxbytes = 1024*100
        with open(filename, 'r') as f:
            got = f.read(maxbytes)
            if not (expect in got):
                if len(got) >= maxbytes:
                    self.fail(filename + ': does not contain expected \'' + expect + '\', or output is too large')
                else:
                    self.fail(filename + ': does not contain expected \'' + expect + '\'')

    def check_empty_file(self, filename):
        """
        Raise an error if the file is not empty
        """
        filesize = os.path.getsize(filename)
        if filesize > 0:
            with open(filename, 'r') as f:
                contents = f.read(1000)
                print 'ERROR: ' + filename + ' expected to be empty, but contains:\n'
                print contents + '...\n'
        self.assertEqual(filesize, 0, filename + ': expected to be empty')

    def check_non_empty_file(self, filename):
        """
        Raise an error if the file is empty
        """
        filesize = os.path.getsize(filename)
        if filesize == 0:
            print 'ERROR: ' + filename + ' should not be empty (this command expected error output)'
        self.assertNotEqual(filesize, 0, filename + ': expected to not be empty')

    # Run the wt utility.
    def runWt(self, args, infilename=None,
        outfilename=None, errfilename=None, reopensession=True, failure=False):

        # Close the connection to guarantee everything is flushed, and that
        # we can open it from another process.
        self.close_conn()

        wtoutname = outfilename or "wt.out"
        wterrname = errfilename or "wt.err"
        with open(wterrname, "w") as wterr:
            with open(wtoutname, "w") as wtout:
                procargs = [os.path.join(wt_builddir, "wt")]
                if self._gdbSubprocess:
                    procargs = [os.path.join(wt_builddir, "libtool"),
                                "--mode=execute", "gdb", "--args"] + procargs
                procargs.extend(args)
                if self._gdbSubprocess:
                    infilepart = ""
                    if infilename != None:
                        infilepart = "<" + infilename + " "
                    print str(procargs)
                    print "*********************************************"
                    print "**** Run 'wt' via: run " + \
                        " ".join(procargs[3:]) + infilepart + \
                        ">" + wtoutname + " 2>" + wterrname
                    print "*********************************************"
                    returncode = subprocess.call(procargs)
                elif infilename:
                    with open(infilename, "r") as wtin:
                        returncode = subprocess.call(
                            procargs, stdin=wtin, stdout=wtout, stderr=wterr)
                else:
                    returncode = subprocess.call(
                        procargs, stdout=wtout, stderr=wterr)
        if failure:
            self.assertNotEqual(returncode, 0,
                'expected failure: "' + \
                str(procargs) + '": exited ' + str(returncode))
        else:
            self.assertEqual(returncode, 0,
                'expected success: "' + \
                str(procargs) + '": exited ' + str(returncode))
        if errfilename == None:
            self.check_empty_file(wterrname)
        if outfilename == None:
            self.check_empty_file(wtoutname)

        # Reestablish the connection if needed
        if reopensession:
            self.open_conn()
