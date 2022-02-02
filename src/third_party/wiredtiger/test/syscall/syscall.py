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
#
# syscall.py
#      Command line syscall test runner
#
# Usage: python syscall.py [ options ]
#
# For each .run file below the current directory, run the corresponding
# program and collect strace output, comparing it to the contents of
# the .run file.
#
# Options:
#    --preserve     preserve the outputs in a WT_TEST.* subdirectory
#                   of the build directory.
#    --verbose      verbose output to show step by step how run files are
#                   compared to output files.

# HOW TO DEBUG FAILURES OR CREATE A NEW TEST
#
# It will be helpful to look at an existing run file (ending in .run)
# while reading this. If you are debugging a failure, also have the
# output file (stderr.txt) available for reference.  These files are
# generated in a WT_TEST.* subdirectory of the build directory and
# preserved in case of a failure, or when the --preserve option is used.
#
# For each run file under this directory, this script runs the program
# built for that directory under the 'strace' program and captures the
# output from that. (On OS/X it runs 'dtruss' instead of 'strace', otherwise
# it is largely the same). We want to compare the strace output to a known
# reference. The program typically has some of its own output, this is
# interleaved into the strace output and provides 'anchor points' during
# the comparison.
#
# The purpose of this output comparison is to determine if there are any
# system calls that we should be doing that are not happening. We'd also catch
# if there are any extra syscalls that we are doing. For example, if we
# are expecting that some operation, like WT_SESSION->create, must do an
# fdatasync at a particular point to enforce durability guarantees,
# it would be pretty bad if a future code change inadvertently stopped
# doing the fdatasync. This wouldn't be picked up by normal testing. It might
# be detected by asynchronously killing a test run and seeing if a
# recovered database gives proper results. Or it might not. This script
# attempts to add certainty to our guarantees.
#
# The run file is a template for what the resulting strace output should
# look like. The challenge is that seemingly minor changes to WiredTiger
# implementation or even runtime libraries may change what the overall output
# looks like. The run file can be written to allow runs that are resilient
# against such changes.
#
# This script's first action is to read the run file after it is run through
# the 'cpp' preprocessor. That means that the run file can use #ifdefs,
# #defines and #includes, as well as /**/ and // comments. The output
# of the preprocessor is then parsed. We expect to see a few directives
# first, each has a string argument as described here:
#
#  SYSTEM(".....");      to tell us what system the script can run on, the arg
#                        currently is either "Linux" and "Darwin".
#  TRACE(".....");       a comma separated list of system calls that
#                        we are looking at. Other system calls are ignored.
#  RUN("");              arguments to the executable.
#
# When the RUN directive is seen, it indicates that this header portion is
# complete, there are no more directives. At this point, the target program
# is executed via strace. The remaining part of the run file is used to
# match the output of strace.
#
# The string '...' in the run file matches anything, and can be used to skip
# over system dependent parts of the strace output.  If '...' appears on a line
# by itself, it matches any number of lines. If it appears immediately after a
# string, it matches a string that begins with the pattern. (e.g. "foo"...
# matches any string that starts with "foo"). It can also appear as a function
# argument where it matches any number of arguments.
#
# Lines of strace generally look something like:
#  open("./WiredTiger.lock", O_RDWR|O_CREAT|O_CLOEXEC, 0666) = 3
#
# where the result of the syscall appears at the end.  A matching line in
# a run file could look like this:
#  fd = open("./WiredTiger.lock", O_RDWR|O_CREAT|O_CLOEXEC, 0666);
#
# or:
#  fd = open("./WiredTiger"..., O_RDWR|O_CREAT|O_CLOEXEC, 0666);
#
# or:
#  fd = open("./WiredTiger"..., ...);
#
# In each of these cases, the 'fd' (which can be any variable name) becomes
# bound to the value in the strace output, in this case '3'. So if later the
# run file contains:
#  write(fd, ""..., 20);
#
# then we would expect this to match strace output for a write of 20 bytes
# using file descriptor 3.
#
# Expressions are evaluated using the Python parser, so that
# hex and octal numbers are accepted, and constant values can be or-ed.
# Some limited number of defines are known (see 'defines_used' below),
# so that the run file can contain 'O_RDONLY' and it will match a numeric
# expression (as it appears in the output of dtruss on OS/X).

from __future__ import print_function
import argparse, distutils.spawn, fnmatch, os, platform, re, shutil, \
    subprocess, sys

# A class that represents a context in which predefined constants can be
# set, and new variables can be assigned.
class VariableContext(object):
    def __getitem__(self, key):
        if key not in dir(self) or key.startswith('__'):
            raise KeyError(key)
        return getattr(self, key)

    def __setitem__(self, key, value):
        setattr(self, key, value)

################################################################
# Changable parameters
# We expect these values to evolve as tests are added or modified.

# Generally, system calls must be wrapped in an ASSERT_XX() "macro".
# Exceptions are calls in this list that return 0 on success, or
# those that are hardcoded in Runner.call_compare()
calls_returning_zero = [ 'close', 'ftruncate', 'fdatasync', 'rename' ]

# Encapsulate all the defines we can use in our scripts.
# When this program is run, we'll find out their actual values on
# the host system.
defines_used = [
    'HAVE_FTRUNCATE', 'O_ACCMODE', 'O_APPEND', 'O_ASYNC',
    'O_CLOEXEC', 'O_CREAT', 'O_EXCL', 'O_EXLOCK', 'O_NOATIME',
    'O_NOFOLLOW', 'O_NONBLOCK', 'O_RDONLY', 'O_RDWR', 'O_SHLOCK',
    'O_TRUNC', 'O_WRONLY', 'WT_USE_OPENAT' ]

################################################################

# Patterns that are used to match the .run file and/or the output.
ident = r'([a-zA-Z_][a-zA-Z0-9_]*)'
outputpat = re.compile(r'OUTPUT\("([^"]*)"\)')
argpat = re.compile(r'''((?:[^,"']|"[^"]*"|'[^']*')+)''')
discardpat = re.compile(r';')

# e.g. fd = open("blah", 0, 0);
assignpat = re.compile(ident + r'\s*=\s*' + ident + r'(\([^;]*\));')

# e.g. ASSERT_EQ(close(fd), 0);
assertpat = re.compile(r'ASSERT_([ENLG][QET])\s*\(\s*' + ident + r'\s*(\(.*\))\s*,\s*([a-zA-Z0-9_]+)\);')

# e.g. close(fd);     must return 0
callpat = re.compile(ident + r'(\(.*\));')

# e.g. open("blah", 0x0, 0x0)   = 6 0
# We capture the errno (e.g. "0" or "Err#60"), but don't do anything with it.
# We don't currently test anything that is errno dependent.
dtruss_pat = re.compile(ident + r'(\(.*\))\s*=\s*(-*[0-9xA-F]+)\s+([-A-Za-z#0-9]*)')
# At the top of the dtruss output is a fixed string.
dtruss_init_pat = re.compile(r'\s*SYSCALL\(args\)\s*=\s*return\s*')

strace_pat = re.compile(ident + r'(\(.*\))\s*=\s(-*[0-9]+)()')

tracepat = re.compile(r'TRACE\("([^"]*)"\)')
runpat = re.compile(r'RUN\(([^\)]*)\)')
systempat = re.compile(r'SYSTEM\("([^"]*)"\)')
# If tracepat matches, set map['trace_syscalls'] to the 0'th group, etc.
headpatterns = [ [ tracepat, 'trace_syscalls', 0],
                 [ systempat, 'required_system', 0],
                 [ runpat, 'run_args', 0] ]

pwrite_in = r'pwrite64'
pwrite_out = r'pwrite'

# To create breakpoints while debugging this script
def bp():
    import pdb
    pdb.set_trace()

def msg(s):
    print("syscall.py: " + s, file=sys.stderr)

def die(s):
    msg(s)
    sys.exit(1)

# If wttop appears as a prefix of pathname, strip it off.
def simplify_path(wttop, pathname):
    wttop = os.path.join(wttop, "")
    if pathname.startswith(wttop):
        pathname = pathname[len(wttop):]
    return pathname

def printfile(pathname, abbrev):
    print("================================================================")
    print(abbrev + " (" + pathname + "):")
    with open(pathname, 'r') as f:
        shutil.copyfileobj(f, sys.stdout)
    print("================================================================")

# A line from a file: a modified string with the file name and line number
# associated with it.
class FileLine(str):
    filename = None
    linenum = 0
    def __new__(cls, filename, linenum, value, *args, **kwargs):
        result = super(FileLine, cls).__new__(cls, value)
        result.filename = filename
        result.linenum = linenum
        return result

    def prefix(self):
        return self.filename + ':' + str(self.linenum) + ': '

    def range_prefix(self, otherline):
        if self == otherline:
            othernum = ''
        elif otherline == None:
            othernum = '-EOF'
        else:
            othernum = '-' + str(otherline.linenum)
        return self.filename + ':' + str(self.linenum) + othernum + ': '

    def normalize(self):
        changed = re.sub(pwrite_in, pwrite_out, self)
        if changed == self:
            normalized = self
        else:
            normalized = FileLine(self.filename, self.linenum, str(changed))
        return normalized

# Manage reading from a file, tracking line numbers.
class Reader(object):
    # 'raw' means we don't ignore any lines
    # 'is_cpp' means input lines beginning with '#' indicate file/linenumber
    def __init__(self, wttop, filename, f, raw = True, is_cpp = False):
        self.wttop = wttop
        self.orig_filename = filename
        self.filename = filename
        self.f = f
        self.linenum = 1
        self.raw = raw
        self.is_cpp = is_cpp
        self.context = []
        if not self.f:
            die(self.filename + ': cannot open')

    def __enter__(self):
        if not self.f:
            return False
        return self

    def __exit__(self, typ, value, traceback):
        if self.f:
            self.f.close()
            self.f = None

    # Return True if the line is to be ignored.
    def ignore(self, line):
        if self.raw:
            return False
        return line == ''

    # strip a line of comments
    def strip_line(self, line):
        if not line:
            return None
        line = line.strip()
        if self.is_cpp and line.startswith('#'):
            parts = line.split()
            if len(parts) < 3 or not parts[1].isdigit():
                msg('bad cpp input: ' + line)
            line = ''
            self.linenum = int(parts[1]) - 1
            self.filename = parts[2].strip('"')
            if self.filename == '<stdin>':
                self.filename = self.orig_filename
        if '//' in line:
            if line.startswith('//'):
                line = ''
            else:
                # This isn't exactly right, it would see "; //"
                # within a string or comment.
                m = re.match(r'^(.*;|.*\.\.\.)\s*//', line)
                if m:
                    line = m.groups()[0].strip()
        return line

    def readline(self):
        rawline = self.f.readline()
        line = self.strip_line(rawline)
        self.add_context(rawline)
        while line != None and self.ignore(line):
            self.linenum += 1
            rawline = self.f.readline()
            line = self.strip_line(rawline)
            self.add_context(rawline)
        if line:
            line = FileLine(self.filename, self.linenum, line)
            self.linenum += 1
        else:
            line = ''     # make this somewhat compatible with file.readline
        return line

    def get_context(self):
        s = ''
        for line in self.context:
            s += '  ' + str(line)
        return s

    def add_context(self, line):
        self.context.append(str(self.linenum) + ': ' + line)
        self.context = self.context[-5:]

    def close(self):
        self.f.close()

# Read from a regular file.
class FileReader(Reader):
    def __init__(self, wttop, filename, raw = True):
        return super(FileReader, self).__init__(wttop, filename,
                                                open(filename), raw, False)

# Read from the C preprocessor run on a file.
class PreprocessedReader(Reader):
    def __init__(self, wttop, filename, predefines, raw = True):
        sourcedir = os.path.dirname(filename)
        cmd = ['cc', '-E', '-I' + sourcedir]
        for name in dir(predefines):
            if not name.startswith('__'):
                cmd.append('-D' + name + '=' + str(predefines[name]))
        cmd.append('-')
        proc = subprocess.Popen(cmd, stdin=open(filename),
            stdout=subprocess.PIPE, universal_newlines=True)
        super(PreprocessedReader, self).__init__(wttop, filename,
                                                 proc.stdout, raw, True)

# Track options discovered in the 'head' section of the .run file.
class HeadOpts:
    def __init__(self):
        self.run_args = None
        self.required_system = None
        self.trace_syscalls = None

# Manage a run of the target program characterized by a .run file,
# comparing output from the run and reporting differences.
class Runner:
    def __init__(self, wttopdir, runfilename, exedir, testexe,
                 strace, args, variables, defines):
        self.variables = variables
        self.defines = defines
        self.wttopdir = wttopdir
        self.runfilename = runfilename
        self.testexe = testexe
        self.exedir = exedir
        self.strace = strace
        self.args = args
        self.headopts = HeadOpts()
        self.dircreated = False
        self.strip_syscalls = None
        outfilename = args.outfilename
        errfilename = args.errfilename
        if outfilename == None:
            self.outfilename = os.path.join(exedir, 'stdout.txt')
        else:
            self.outfilename = outfilename
        if errfilename == None:
            self.errfilename = os.path.join(exedir, 'stderr.txt')
        else:
            self.errfilename = errfilename

        self.runfile = PreprocessedReader(self.wttopdir, runfilename,
                                          self.defines, False)

    def init(self, systemtype):
        # Read up until 'RUN()', setting attributes of self.headopts
        runline = '?'
        m = None
        while runline:
            runline = self.runfile.readline()
            m = None
            for pat,attr,group in headpatterns:
                m = re.match(pat, runline)
                if m:
                    setattr(self.headopts, attr, m.groups()[group])
                    break
            if not m:
                self.fail(runline, "unknown header option: " + runline)
                return [ False, False ]
            if self.headopts.run_args != None:   # found RUN()?
                break
        if not self.headopts.trace_syscalls:
            msg("'" + self.runfile.filename + "': needs TRACE(...)")
            return [ False, False ]
        runargs = self.headopts.run_args.strip()
        if len(runargs) > 0:
            if len(runargs) < 2 or runargs[0] != '"' or runargs[-1] != '"':
                msg("'" + self.runfile.filename +
                    "': Missing double quotes in RUN arguments")
                return [ False, False ]
            runargs = runargs[1:-1]
        self.runargs = runargs.split()
        #print("SYSCALLS: " + self.headopts.trace_syscalls
        if self.headopts.required_system != None and \
            self.headopts.required_system != systemtype:
            msg("skipping '" + self.runfile.filename + "': for '" +
                self.headopts.required_system + "', this system is '"
                + systemtype + "'")
            return [ False, True ]
        return [ True, False ]

    def close(self, forcePreserve):
        self.runfile.close()
        if self.exedir and self.dircreated and \
           not self.args.preserve and not forcePreserve:
            os.chdir('..')
            shutil.rmtree(self.exedir)

    def fail(self, line, s):
        # make it work if line is None or is a plain string.
        try:
            prefix = simplify_path(self.wttopdir, line.prefix())
        except:
            prefix = 'syscall.py: '
        print(prefix + s, file=sys.stderr)

    def failrange(self, errfile, line, lineto, s):
        # make it work if line is None or is a plain string.
        try:
            prefix = simplify_path(self.wttopdir, line.range_prefix(lineto))
        except:
            prefix = 'syscall.py: '
        print(prefix + s + '\n' + errfile.get_context(), file=sys.stderr)

    def str_match(self, s1, s2):
        fuzzyRight = False
        if len(s1) < 2 or len(s2) < 2:
            return False
        if s1[-3:] == '...':
            fuzzyRight = True
            s1 = s1[:-3]
        if s2[-3:] == '...':
            s2 = s2[:-3]
        if s1[0] != '"' or s1[-1] != '"' or s2[0] != '"' or s2[-1] != '"':
            return False
        s1 = s1[1:-1]
        s2 = s2[1:-1]
        # We allow a trailing \0
        if s1[-2:] == '\\0':
            s1 = s1[:-2]
        if s2[-2:] == '\\0':
            s2 = s2[:-2]
        if fuzzyRight:
            return s2.startswith(s2)
        else:
            return s1 == s2

    def expr_eval(self, s):
        return eval(s, {}, self.variables)

    def arg_match(self, a1, a2):
        a1 = a1.strip()
        a2 = a2.strip()
        if a1 == a2:
            return True
        if len(a1) == 0 or len(a2) == 0:
            return False
        if a1[0] == '"':
            return self.str_match(a1, a2)
        #print('  arg_match: <' + a1 + '> <' + a2 + '>')
        try:
            a1value = self.expr_eval(a1)
        except Exception:
            self.fail(a1, 'unknown expression: ' + a1)
            return False
        try:
            a2value = self.expr_eval(a2)
        except Exception:
            self.fail(a2, 'unknown expression: ' + a2)
            return False
        return a1value == a2value or int(a1value) == int(a2value)

    def split_args(self, s):
        if s[0] == '(':
            s = s[1:]
        if s[-1] == ')':
            s = s[:-1]
        return argpat.split(s)[1::2]

    def args_match(self, args1, args2):
        #print('args_match: ' + str(s1) + ', ' + str(s2))
        pos = 0
        for a1 in args1:
            a1 = a1.strip()
            if a1 == '...':  # match anything?
                return True
            if pos >= len(args2):
                return False
            if not self.arg_match(a1, args2[pos]):
                return False
            pos += 1
        if pos < len(args2):
            return False
        return True

    # func(args);  is shorthand for ASSERT_EQ(func(args), xxx);
    # where xxx may be 0 or may be derived from one of the args.
    def call_compare(self, callname, result, eargs, errline):
        if callname in calls_returning_zero:
            return self.compare("EQ", result, "0", errline)
        elif callname == 'pwrite' or callname == 'pwrite64':
            return self.compare("EQ",
                re.sub(pwrite_in, pwrite_out, result),
                re.sub(pwrite_in, pwrite_out, eargs[2]),
                errline)
        else:
            self.fail(errline, 'call ' + callname +
                      ': not known, use ASSERT_EQ()')

    def compare(self, compareop, left, right, errline):
        l = self.expr_eval(left)
        r = self.expr_eval(right)
        if (compareop == "EQ" and l == r) or \
           (compareop == "NE" and l != r) or \
           (compareop == "LT" and l < r) or \
           (compareop == "LE" and l <= r) or \
           (compareop == "GT" and l > r) or \
           (compareop == "GE" and l >= r):
            return True
        else:
            self.fail(errline,
                      'call returned value: ' + left + ', comparison: (' +
                      left + ' ' + compareop + ' ' + right +
                      ') at line: ' + errline)
            return False

    def match_report(self, runline, errline, verbose, skiplines, result, desc):
        if result:
            if verbose:
                print('MATCH:')
                print('  ' + runline.prefix() + runline)
                print('    ' + errline.prefix() + errline)
        else:
            if verbose:
                if not skiplines:
                    msg('Expecting ' + desc)
                    print('  ' + runline.prefix() + runline +
                          ' does not match:')
                    print('    ' + errline.prefix() + errline)
                else:
                    print('  (... match) ' + errline.prefix() + errline)
        return result

    def match(self, runline, errline, verbose, skiplines):
        m = re.match(outputpat, runline)
        if m:
            outwant = m.groups()[0]
            return self.match_report(runline, errline, verbose, skiplines,
                                     errline == outwant, 'output line')
        if self.args.systype == 'Linux':
            em = re.match(strace_pat, errline)
        elif self.args.systype == 'Darwin':
            em = re.match(dtruss_pat, errline)
        if not em:
            self.fail(errline, 'Unknown strace/dtruss output: ' + errline)
            return False
        gotcall = em.groups()[0]
        # filtering syscalls here if needed.  If it's not a match,
        # mark the errline so it is retried.
        if self.strip_syscalls != None and gotcall not in self.strip_syscalls:
            errline.skip = True
            return False
        m = re.match(assignpat, runline)
        if m:
            if m.groups()[1] != gotcall:
                return self.match_report(runline, errline, verbose, skiplines,
                                         False, 'syscall to match assignment')

            rargs = self.split_args(m.groups()[2])
            eargs = self.split_args(em.groups()[1])
            result = self.args_match(rargs, eargs)
            if result:
                self.variables[m.groups()[0]] = em.groups()[2]
            return self.match_report(runline, errline, verbose, skiplines,
                                     result, 'syscall to match assignment')

        # pattern groups using example ASSERT_EQ(close(fd), 0);
        #  0   :  comparison op ("EQ")
        #  1   :  function call name "close"
        #  2   :  function call args "(fd)"
        #  3   :  comparitor "0"
        m = re.match(assertpat, runline)
        if m:
            if m.groups()[1] != gotcall:
                return self.match_report(runline, errline, verbose, skiplines,
                                         False, 'syscall to match ASSERT')

            rargs = self.split_args(m.groups()[2])
            eargs = self.split_args(em.groups()[1])
            result = self.args_match(rargs, eargs)
            if not result:
                return self.match_report(runline, errline, verbose, skiplines,
                                         result, 'syscall to match ASSERT')
            result = self.compare(m.groups()[0], em.groups()[2],
                                  m.groups()[3], errline)
            return self.match_report(runline, errline, verbose, skiplines,
                                     result, 'ASSERT')

        # A call without an enclosing ASSERT is reduced to an ASSERT,
        # depending on the particular system call.
        m = re.match(callpat, runline)
        if m:
            if m.groups()[0] != gotcall:
                return self.match_report(runline, errline, verbose, skiplines,
                                         False, 'syscall')

            rargs = self.split_args(m.groups()[1])
            eargs = self.split_args(em.groups()[1])
            result = self.args_match(rargs, eargs)
            if not result:
                return self.match_report(runline, errline, verbose, skiplines,
                                         result, 'syscall')
            result = self.call_compare(m.groups()[0], em.groups()[2],
                                     eargs, errline)
            return self.match_report(runline, errline, verbose, skiplines,
                                     result, 'syscall')

        self.fail(runline, 'unrecognized pattern in runfile:' + runline)
        return False

    def match_lines(self):
        outfile = FileReader(self.wttopdir, self.outfilename, True)
        errfile = FileReader(self.wttopdir, self.errfilename, True)

        if outfile.readline():
            self.fail(None, 'output file has content, expected to be empty')
            return False

        with outfile, errfile:
            runlines = self.order_runfile(self.runfile)
            errline = errfile.readline()
            if re.match(dtruss_init_pat, errline):
                errline = errfile.readline()
            errline = errline.normalize()
            skiplines = False
            for runline in runlines:
                runline = runline.normalize()
                if runline == '...':
                    skiplines = True
                    if self.args.verbose:
                        print('Fuzzy matching:')
                        print('  ' + runline.prefix() + runline)
                    continue
                first_errline = errline
                while errline and not self.match(runline, errline,
                                                 self.args.verbose, skiplines):
                    if skiplines or hasattr(errline, 'skip'):
                        errline = errfile.readline().normalize()
                    else:
                        self.fail(runline, "expecting " + runline)
                        self.failrange(errfile, first_errline, errline,
                                       "does not match")
                        return False
                if not errline:
                    self.fail(runline, "failed to match line: " + runline)
                    self.failrange(errfile, first_errline, errline,
                                   "does not match")
                    return False
                errline = errfile.readline()
                if re.match(dtruss_init_pat, errline):
                    errline = errfile.readline()
                errline = errline.normalize()
                skiplines = False
            if errline and not skiplines:
                self.fail(errline, "extra lines seen starting at " + errline)
                return False
            return True

    def order_runfile(self, f):
        # In OS X, dtruss is implemented using dtrace's apparently buffered
        # printf writes to stdout, but that is all redirected to stderr.
        # Because of that, the test program's writes to stderr do not
        # interleave with dtruss output as it does with Linux's strace
        # (which writes directly to stderr).  On OS X, we get the program's
        # output first, we compensate for this by moving all the
        # OUTPUT statements in the runfile to match first. This simple
        # approach will break if there is more data generated by OUTPUT
        # statements than a stdio buffer's size.
        matchout = (self.args.systype == 'Darwin')
        out = []
        nonout = []
        s = f.readline()
        while s:
            if matchout and re.match(outputpat, s):
                out.append(s)
            elif not re.match(discardpat, s):
                nonout.append(s)
            s = f.readline()
        out.extend(nonout)
        return out

    def run(self):
        if not self.exedir:
            self.fail(None, "Execution directory not set")
            return False
        if not os.path.isfile(self.testexe):
            msg("'" + self.testexe + "': no such file")
            return False

        shutil.rmtree(self.exedir, ignore_errors=True)
        os.mkdir(self.exedir)
        self.dircreated = True
        os.chdir(self.exedir)

        callargs = list(self.strace)
        trace_syscalls = self.headopts.trace_syscalls
        if self.args.systype == 'Linux':
            callargs.extend(['-e', 'trace=' + trace_syscalls ])
        elif self.args.systype == 'Darwin':
            # dtrace has no option to limit the syscalls to be traced,
            # so we'll filter the output.
            self.strip_syscalls = re.sub(pwrite_in, pwrite_out,
                self.headopts.trace_syscalls).split(',')
        callargs.append(self.testexe)
        callargs.extend(self.runargs)

        outfile = open(self.outfilename, 'w')
        errfile = open(self.errfilename, 'w')
        if self.args.verbose:
            print('RUNNING: ' + str(callargs))
        subret = subprocess.call(callargs, stdout=outfile, stderr=errfile)
        outfile.close()
        errfile.close()
        if subret != 0:
            msg("'" + self.testexe + "': exit value " + str(subret))
            printfile(self.outfilename, "output")
            printfile(self.errfilename, "error")
            return False
        return True

# Run the syscall program.
class SyscallCommand:
    def __init__(self, disttop, builddir):
        self.disttop = disttop
        self.builddir = builddir

    def parse_args(self, argv):
        srcdir = os.path.join(self.disttop, 'test', 'syscall')
        self.exetopdir = os.path.join(self.builddir, 'test', 'syscall')
        self.incdirs = []

        if self.disttop == self.builddir:
            # CTest runs a copy of the script in the build directory, so the src include
            # is a level above.
            self.incdirs.append(os.path.join(self.disttop, '..', 'src', 'include'))
        else:
            self.incdirs.append(os.path.join(self.disttop, 'src', 'include'))

        if os.path.isfile(os.path.join(self.builddir, 'wiredtiger_config.h')) and \
            os.path.isfile(os.path.join(self.builddir, 'wiredtiger.h')):
            # When building with autoconf, the generated includes will be at the top level
            # of the build directory.
            self.incdirs.append(self.builddir)
        elif os.path.isfile(os.path.join(self.builddir, 'config', 'wiredtiger_config.h')) and \
            os.path.isfile(os.path.join(self.builddir, 'include', 'wiredtiger.h')):
            # When building with CMake, the generated includes will be in the config and include
            # sub-directories.
            self.incdirs.append(os.path.join(self.builddir, 'config'))
            self.incdirs.append(os.path.join(self.builddir, 'include'))

        ap = argparse.ArgumentParser('Syscall test runner')
        ap.add_argument('--systype',
                        help='override system type (Linux/Windows/Darwin)')
        ap.add_argument('--errfile', dest='errfilename',
                        help='do not run the program, use this file as stderr')
        ap.add_argument('--outfile', dest='outfilename',
                        help='do not run the program, use this file as stdout')
        ap.add_argument('--preserve', action="store_true",
                        help='keep the WT_TEST.* directories')
        ap.add_argument('--verbose', action="store_true",
                        help='add some verbose information')
        ap.add_argument('tests', nargs='*',
                        help='the tests to run (defaults to all)')
        args = ap.parse_args()

        if not args.systype:
            args.systype = platform.system()   # Linux, Windows, Darwin

        self.dorun = True
        if args.errfilename or args.outfilename:
            if len(args.tests) != 1:
                msg("one test is required when --errfile or --outfile" +
                    " is specified")
                return False
            if not args.outfilename:
                args.outfilename = os.devnull
            if not args.errfilename:
                args.errfilename = os.devnull
            self.dorun = False

        # for now, we permit Linux and Darwin
        straceexe = None
        if args.systype == 'Linux':
            strace = [ 'strace' ]
            straceexe = 'strace'
        elif args.systype == 'Darwin':
            strace = [ 'sudo', 'dtruss' ]
            straceexe = 'dtruss'
        else:
            msg("systype '" + args.systype + "' unsupported")
            return False
        if not distutils.spawn.find_executable(straceexe):
            msg("strace: does not exist")
            return False
        self.args = args
        self.strace = strace
        return True

    def runone(self, runfilename, exedir, testexe, args):
        result = True
        runner = Runner(self.disttop, runfilename, exedir, testexe,
                        self.strace, args, self.variables, self.defines)
        okay, skip = runner.init(args.systype)
        if not okay:
            if not skip:
                result = False
        else:
            if testexe:
                print('running ' + testexe)
                if not runner.run():
                    result = False
            if result:
                print('comparing:')
                print('  ' + simplify_path(self.disttop, runfilename))
                print('  ' + simplify_path(self.disttop, runner.errfilename))
                result = runner.match_lines()
                if not result and args.verbose:
                    printfile(runfilename, "runfile")
                    printfile(runner.errfilename, "trace output")
        runner.close(not result)
        if not result:
            print('************************ FAILED ************************')
            print('  see results in ' + exedir)
        print('')
        return result

    # Create a C program to get values for all defines we need.
    # The output of the program is Python code that we'll execute
    # directly to set the values.
    def build_system_defines(self):
        # variables is a symbol table that is used to
        # evaluate expressions both in the .run file and
        # in the output file. This is needed for strace,
        # which shows system call flags in symbolic form.
        self.variables = VariableContext()
        # defines is a symbol table that is used to
        # create preprocessor defines, effectively evaluating
        # all flag defines in the .run file.
        self.defines = VariableContext()
        program = \
            '#include <stdio.h>\n' + \
            '#include <fcntl.h>\n' + \
            '#include <wt_internal.h>\n' + \
            'int main() {\n'
        for define in defines_used:
            program += '#ifdef ' + define + '\n'
            # output is Python that sets attributes of 'o'.
            program += '  printf("o.' + define + '=%d\\n", ' + \
                       define + ');\n'
            program += '#endif\n'
        program += \
            '  return(0);\n' + \
            '}\n'
        probe_c = os.path.join(self.exetopdir, "syscall_probe.c")
        probe_exe = os.path.join(self.exetopdir, "syscall_probe")
        with open(probe_c, "w") as f:
            f.write(program)
        ccargs = ['cc', '-o', probe_exe]
        for inc in self.incdirs:
            ccargs.append('-I' + inc)
        if self.args.systype == 'Linux':
            ccargs.append('-D_GNU_SOURCE')
        ccargs.append(probe_c)
        subret = subprocess.call(ccargs)
        if subret != 0:
            msg("probe compilation returned " + str(subret))
            return False
        proc = subprocess.Popen([probe_exe], stdout=subprocess.PIPE,
            universal_newlines=True)
        out, err = proc.communicate()
        subret = proc.returncode
        if subret != 0 or err:
            msg("probe run returned " + str(subret) + ", error=" + str(err))
            return False
        if self.args.verbose:
            print('probe output:\n' + out)
        o = self.defines     # The 'o' object will be modified.
        exec(out)            # Run the produced Python.
        o = self.variables   # Set these in variables too, so strace
        exec(out)            #  symbolic output is evaluated.
        if not self.args.preserve:
            os.remove(probe_c)
            os.remove(probe_exe)
        return True

    def execute(self):
        args = self.args
        result = True
        if not self.build_system_defines():
            die('cannot build system defines')
        if not self.dorun:
            for testname in args.tests:
                result = self.runone(testname, None, None, args) and result
        else:
            if len(args.tests) > 0:
                tests = []
                for arg in args.tests:
                    abspath = os.path.abspath(arg)
                    tests.append([os.path.dirname(abspath), [],
                                  [os.path.basename(abspath)]])
            else:
                tests = os.walk(syscalldir)
            os.chdir(self.exetopdir)
            for path, subdirs, files in tests:
                testnum = -1 if len(files) <= 1 else 0
                for name in files:
                    if fnmatch.fnmatch(name, '*.run'):
                        testname = os.path.basename(os.path.normpath(path))
                        runfilename = os.path.join(path, name)
                        testexe = os.path.join(self.exetopdir,
                                               'test_' + testname)
                        exedir = os.path.join(self.exetopdir,
                                              'WT_TEST.' + testname)
                        # If there are multiple tests in this directory,
                        # give each one its own execution dir.
                        if testnum >= 0:
                            exedir += '.' + str(testnum)
                            testnum += 1
                        result = self.runone(runfilename, exedir,
                                             testexe, args) and result
        return result

# Set paths, determining the top of the build.
syscalldir = sys.path[0]
wt_disttop = os.path.dirname(os.path.dirname(syscalldir))

# Note: this code is borrowed from test/suite/run.py
# Check for a local build that contains the wt utility. First check if the user
# manually specified a local build through the 'WT_BUILDDIR' env variable. Otherwise
# iterate through other possible locations. This including current working directory,
# and in the dist directory. This isn't ideal - if a
# user has multiple builds in a tree we could pick the wrong one.
env_builddir = os.getenv('WT_BUILDDIR')
if env_builddir and os.path.isfile(os.path.join(env_builddir, 'wt')):
    wt_builddir = env_builddir
elif os.path.isfile(os.path.join(os.getcwd(), 'wt')):
    wt_builddir = os.getcwd()
elif os.path.isfile(os.path.join(wt_disttop, 'wt')):
    wt_builddir = wt_disttop
elif os.path.isfile(os.path.join(wt_disttop, 'wt.exe')):
    wt_builddir = wt_disttop
else:
    die('unable to find useable WiredTiger build')

cmd = SyscallCommand(wt_disttop, wt_builddir)
if not cmd.parse_args(sys.argv):
    die('bad usage')
if not cmd.execute():
    print('For a HOW TO on debugging, see the top of syscall.py',
          file=sys.stderr)
    sys.exit(1)
sys.exit(0)
