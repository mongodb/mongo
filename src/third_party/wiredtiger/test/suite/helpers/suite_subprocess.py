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
import os, re, signal, subprocess, sys
from run import wt_builddir
from wttest import WiredTigerTestCase
import wttest

# suite_subprocess.py
#    Run a subprocess within the test suite
# Used as a 'mixin' class along with a WiredTigerTestCase class
class suite_subprocess:
    subproc = None
    """
    Check the first 1GB content from a file.
    """
    maxbytes = 1 * 1024 ** 3

    # The signal __wt_debug_crash raises, for callers to hand to assert_crashed. Windows has no
    # SIGKILL: __wt_debug_crash aborts there instead, and assert_crashed does not compare the
    # signal on that platform, so the fallback describes the abort without ever being used.
    debug_crash_signal = getattr(signal, 'SIGKILL', signal.SIGABRT)

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
        with open(filename, 'r') as f:
            got = f.read(self.maxbytes)
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
                if len(got) >= self.maxbytes:
                    self.fail(filename + ': does not contain expected ' + expect + ', or output is larger than ' + str(self.maxbytes) + ' Bytes')
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
            with open(filename, 'r') as f:
                contents = f.read(self.maxbytes)
                if len(contents) > 0:
                    if len(contents) >= self.maxbytes:
                        contents += '...\n'
                    sepline = '*' * 50 + '\n'
                    out = sepline + filename + '\n' + sepline + contents
                    WiredTigerTestCase.prout(out)

    # Run a method as a subprocess using the run.py machinery.
    # Return the process exit status and the WiredTiger home
    # directory used by the subprocess.
    def run_subprocess_function(self, directory, funcname, silent=False, scenario=None):
        testparts = funcname.split('.')
        if len(testparts) != 3:
            raise ValueError('bad function name "' + funcname +
                '", should be three part dotted name')
        topdir = os.path.dirname(self.buildDirectory())
        runscript = os.path.join(topdir, 'test', 'suite', 'run.py')
        # Restrict the subprocess to a single scenario if specified, so that each scenario is
        # exercised (and asserted) independently.
        scenario_args = [ '-s', str(scenario) ] if scenario is not None else []
        # The subprocess runs under the same hooks as this process, otherwise it would exercise
        # none of what the hooks are meant to exercise, and a hook that alters the database layout
        # would leave us unable to reopen what the subprocess wrote.
        #
        # A table the subprocess creates under the disagg hook is layered, but the hook tracks that
        # per test case rather than per home directory, so reading it back as 'table:' in this
        # process still fails. See FIXME-WT-16920 in hook_disagg.py.
        hook_args = [ arg for spec in self.hook_specs for arg in [ '--hook', spec ] ]
        # A hook that skips the function in the subprocess would otherwise leave us nothing to
        # look at: the subprocess exits zero and the skipped test removes its home directory. The
        # report file cannot live under the directory, which run.py clears as it starts.
        skipfile = directory + '.skip'
        if os.path.exists(skipfile):
            os.remove(skipfile)
        procargs = [ sys.executable, runscript, '-p', '--dir', directory,
            '--skip-report', skipfile, *hook_args, *scenario_args, funcname]

        returncode = -1
        os.makedirs(directory)
        # We cannot put the output/error files in the subdirectory, as
        # that will be cleared by the run.py script.
        with open("subprocess.err", "w") as wterr:
            with open("subprocess.out", "w") as wtout:
                returncode = subprocess.call(
                    procargs, stdout=wtout, stderr=wterr)
                if returncode != 0 and not silent:
                    # This is not necessarily an error, the primary reason to
                    # run in a subprocess is that it may crash.
                    self.show_outputs(procargs,
                        "Warning: run_subprocess_function " + funcname + \
                        " returned error code " + str(returncode),
                        [ "subprocess.out", "subprocess.err" ])

        # Whatever stopped the subprocess function from running applies to this test as well.
        if os.path.exists(skipfile):
            with open(skipfile, 'r') as f:
                reason = f.read()
            self.skipTest(funcname + ' was skipped in the subprocess: ' + reason)

        # Running a scenario will default create directory starting with 0.
        new_home_dir = os.path.join(directory,
            testparts[1] + '.0')
        return [ returncode, new_home_dir ]

    # Assert that a subprocess was stopped by the given signal. WiredTiger raises the signal on
    # POSIX and aborts on Windows, so the exit status is platform specific. Where the two are
    # distinguishable, insist on the signal: any other one means the subprocess stopped somewhere
    # it was not asked to, such as an assertion catching a crash point that was configured but
    # never reached.
    def assert_crashed(self, returncode, expected_signal):
        if os.name == 'nt':
            self.assertNotEqual(returncode, 0)
        else:
            self.assertEqual(returncode, -expected_signal)

    # Run a method as a subprocess that is expected to crash, and return the WiredTiger home
    # directory it left behind. The signal differs by how WiredTiger stops: __wt_debug_crash kills
    # the process, an assertion or a panic aborts it.
    def crash_in_subprocess(self, directory, funcname, expected_signal):
        # Restrict the subprocess to this test's own scenario, so that each is crashed and asserted
        # independently. Running the child over the whole list instead crashes it in the first
        # scenario and never reaches the others. A test without scenarios has no attribute, and
        # run_subprocess_function takes None to mean it should not restrict the child at all.
        [ returncode, home ] = self.run_subprocess_function(directory, funcname, silent=True,
            scenario=getattr(self, 'scenario_number', None))
        self.assert_crashed(returncode, expected_signal)
        return home

    # Merge a connection configuration string into a wt argument list, combining with an existing
    # -C value when present.
    def _add_wt_conn_config(self, args, conn_config):
        args = list(args)
        if '-C' in args:
            value = args.index('-C') + 1
            args[value] = '%s,%s' % (args[value], conn_config)
        else:
            args = ['-C', conn_config] + args
        return args

    # Run the wt utility.

    # FIXME-WT-9808:
    # The tiered hook silently interjects tiered configuration and extensions,
    # these are not yet dealt with when running the external 'wt' process.
    @wttest.skip_for_hook("tiered", "runWt cannot add needed extensions")
    def runWt(self, args, infilename=None,
        outfilename=None, errfilename=None, closeconn=True,
        reopensession=True, failure=False):

        if 'timestamp' in self.hook_names and args[0] == 'load':
            self.skipTest("the load utility cannot be run when timestamps are already set")

        # If disagg and verify, change table and file URIs to layered
        if 'disagg' in self.hook_names and 'verify' in args:
            args = [
                re.sub(r'''             # Raw string with extended regex
                       ^                # Line beginning
                       (?:table|file):  # "table:" or "file:" prefix, non-capturing
                       (.*?)            # Non-greedy capture of the name
                       (?:\.wt)?        # Optional ".wt" suffix, non-capturing. Must be stripped out when using layered URI
                       $                # Line end
                       ''', r'layered:\1', a, flags=re.X)
                for a in args
            ]

        # Pass on any extensions a hook saved in hook_extensions.
        ext_config = getattr(self, 'hook_extensions', None)
        if ext_config is not None:
            args = self._add_wt_conn_config(args, ext_config)

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
