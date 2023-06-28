#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
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

from __future__ import print_function
import os, re, subprocess, sys
from run import wt_builddir
from wttest import WiredTigerTestCase
import wttest

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
            print('**************** ' + match + ' in output file: ' + filename + ' ****************')
            if hasPrevious:
                print('...')
            for line in lines:
                print(line, end=' ')
            if hasNext:
                print('...')
            print('********************************')
            self.fail('ERROR found in output file: ' + filename)

    # If the string is of the form '/.../', then return just the embedded
    # pattern, otherwise, return None
    def convert_to_pattern(self, s):
        if len(s) >= 2 and s[0] == '/' and s[-1] == '/':
            return s[1:-1]
        else:
            return None

    def check_file_content(self, filename, expect):
        with open(filename, 'r') as f:
            got = f.read(len(expect) + 100)
            self.assertEqual(got, expect, filename + ': does not contain expected:\n\'' + expect + '\', but contains:\n\'' + got + '\'.')

    # Check contents of the file against a provided checklist. Expected is used as a bool to either
    # ensure checklist is included or ensure the checklist is not included in the file.
    def check_file_contains_one_of(self, filename, checklist, expected):
        """
        Check that the file contains the expected string in the first 100K bytes
        """
        maxbytes = 1024*100
        with open(filename, 'r') as f:
            got = f.read(maxbytes)
            found = False
            for expect in checklist:
                pat = self.convert_to_pattern(expect)
                if pat == None:
                    if expect in got:
                        found = True
                        if expected:
                            break
                        else:
                            self.fail("Did not expect: " + got)
                else:
                    if re.search(pat, got):
                        found = True
                        if expected:
                            break
                        else:
                            self.fail("Did not expect: " + got)
            if not found and expected:
                if len(checklist) == 1:
                    expect = '\'' + checklist[0] + '\''
                else:
                    expect = str(checklist)
                gotstr = '\'' + \
                    (got if len(got) < 1000 else (got[0:1000] + '...')) + '\''
                if len(got) >= maxbytes:
                    self.fail(filename + ': does not contain expected ' + expect + ', or output is too large, got ' + gotstr)
                else:
                    self.fail(filename + ': does not contain expected ' + expect + ', got ' + gotstr)

    def check_file_contains(self, filename, content):
        self.check_file_contains_one_of(filename, [content], True)

    def check_file_not_contains(self, filename, content):
        self.check_file_contains_one_of(filename, [content], False)

    def check_empty_file(self, filename):
        """
        Raise an error if the file is not empty
        """
        filesize = os.path.getsize(filename)
        if filesize > 0:
            with open(filename, 'r') as f:
                contents = f.read(1000)
                print('ERROR: ' + filename + ' expected to be empty, but contains:\n')
                print(contents + '...\n')
        self.assertEqual(filesize, 0, filename + ': expected to be empty')

    def check_non_empty_file(self, filename):
        """
        Raise an error if the file is empty
        """
        filesize = os.path.getsize(filename)
        if filesize == 0:
            print('ERROR: ' + filename + ' should not be empty (this command expected error output)')
        self.assertNotEqual(filesize, 0, filename + ': expected to not be empty')

    def verbose_env(self, envvar):
        return envvar + '=' + str(os.environ.get(envvar)) + '\n'

    def show_outputs(self, procargs, message, filenames):
        out = message + ': ' + \
              str(procargs) + '\n' + \
              self.verbose_env('PATH') + \
              self.verbose_env('LD_LIBRARY_PATH') + \
              self.verbose_env('DYLD_LIBRARY_PATH') + \
              self.verbose_env('PYTHONPATH') + \
              'output files follow:'
        WiredTigerTestCase.prout(out)
        for filename in filenames:
            maxbytes = 1024*100
            with open(filename, 'r') as f:
                contents = f.read(maxbytes)
                if len(contents) > 0:
                    if len(contents) >= maxbytes:
                        contents += '...\n'
                    sepline = '*' * 50 + '\n'
                    out = sepline + filename + '\n' + sepline + contents
                    WiredTigerTestCase.prout(out)

    # Run a method as a subprocess using the run.py machinery.
    # Return the process exit status and the WiredTiger home
    # directory used by the subprocess.
    def run_subprocess_function(self, directory, funcname):
        testparts = funcname.split('.')
        if len(testparts) != 3:
            raise ValueError('bad function name "' + funcname +
                '", should be three part dotted name')
        topdir = os.path.dirname(self.buildDirectory())
        runscript = os.path.join(topdir, 'test', 'suite', 'run.py')
        procargs = [ sys.executable, runscript, '-p', '--dir', directory,
            funcname]

        # scenario_number is only set if we are running in a scenario
        try:
            scennum = self.scenario_number
            procargs.append('-s')
            procargs.append(str(scennum))
        except:
            scennum = 0

        returncode = -1
        os.makedirs(directory)

        # We cannot put the output/error files in the subdirectory, as
        # that will be cleared by the run.py script.
        with open("subprocess.err", "w") as wterr:
            with open("subprocess.out", "w") as wtout:
                returncode = subprocess.call(
                    procargs, stdout=wtout, stderr=wterr)
                if returncode != 0:
                    # This is not necessarily an error, the primary reason to
                    # run in a subprocess is that it may crash.
                    self.show_outputs(procargs,
                        "Warning: run_subprocess_function " + funcname + \
                        " returned error code " + str(returncode),
                        [ "subprocess.out", "subprocess.err" ])

        new_home_dir = os.path.join(directory,
            testparts[1] + '.' + str(scennum))
        return [ returncode, new_home_dir ]

    # Run the wt utility.

    # FIXME-WT-9808:
    # The tiered hook silently interjects tiered configuration and extensions,
    # these are not yet dealt with when running the external 'wt' process.
    @wttest.skip_for_hook("tiered", "runWt cannot add needed extensions")
    def runWt(self, args, infilename=None,
        outfilename=None, errfilename=None, closeconn=True,
        reopensession=True, failure=False):

        # FIXME-WT-9809:
        if 'timestamp' in self.hook_names and args[0] == 'load':
            self.skipTest("the load utility cannot be run when timestamps are already set")

        # Close the connection to guarantee everything is flushed, and that
        # we can open it from another process.
        if closeconn:
            self.close_conn()

        wtoutname = outfilename or "wt.out"
        wterrname = errfilename or "wt.err"
        with open(wterrname, "w") as wterr:
            with open(wtoutname, "w") as wtout:
                # Prefer running the actual 'wt' binary rather than the
                # 'wt' script created by libtool. On OS/X with System Integrity
                # Protection enabled, running a shell script strips
                # environment variables needed to run 'wt'. There are
                # also test environments that work better with the binary.
                libs_wt = os.path.join(wt_builddir, ".libs", "wt")
                if os.path.isfile(libs_wt):
                    wtexe = libs_wt
                else:
                    wtexe = os.path.join(wt_builddir, "wt")
                procargs = [ wtexe ]
                if self._gdbSubprocess:
                    procargs = [ "gdb", "--args" ] + procargs
                elif self._lldbSubprocess:
                    procargs = [ "lldb", "--" ] + procargs
                procargs.extend(args)
                if self._gdbSubprocess:
                    infilepart = ""
                    if infilename != None:
                        infilepart = "<" + infilename + " "
                    print(str(procargs))
                    print("*********************************************")
                    print("**** Run 'wt' via: run " + \
                        " ".join(procargs[3:]) + infilepart + \
                        ">" + wtoutname + " 2>" + wterrname)
                    print("*********************************************")
                    returncode = subprocess.call(procargs)
                elif self._lldbSubprocess:
                    infilepart = ""
                    if infilename != None:
                        infilepart = "<" + infilename + " "
                    print(str(procargs))
                    print("*********************************************")
                    print("**** Run 'wt' via: run " + \
                        " ".join(procargs[3:]) + infilepart + \
                        ">" + wtoutname + " 2>" + wterrname)
                    print("*********************************************")
                    returncode = subprocess.call(procargs)
                elif infilename:
                    with open(infilename, "r") as wtin:
                        returncode = subprocess.call(
                            procargs, stdin=wtin, stdout=wtout, stderr=wterr)
                else:
                    returncode = subprocess.call(
                        procargs, stdout=wtout, stderr=wterr)
        if failure:
            if returncode == 0:
                self.show_outputs(procargs,
                    "ERROR: wt command expected failure, got success",
                    [wtoutname, wterrname])
            self.assertNotEqual(returncode, 0,
                'expected failure: "' + \
                str(procargs) + '": exited ' + str(returncode))
        else:
            if returncode != 0:
                self.show_outputs(procargs,
                    "ERROR: wt command expected success, got failure",
                    [wtoutname, wterrname])
            self.assertEqual(returncode, 0,
                'expected success: "' + \
                str(procargs) + '": exited ' + str(returncode))
        if errfilename == None:
            self.check_empty_file(wterrname)
        if outfilename == None:
            self.check_empty_file(wtoutname)

        # Reestablish the connection if needed
        if reopensession and closeconn:
            self.open_conn()
