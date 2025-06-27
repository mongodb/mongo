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

"""Code for debugging SCons internal things.

Shouldn't be needed by most users. Quick shortcuts::

    from SCons.Debug import caller_trace
    caller_trace()
"""

import atexit
import os
import sys
import time
import weakref
import inspect

# Global variable that gets set to 'True' by the Main script,
# when the creation of class instances should get tracked.
track_instances = False
# List of currently tracked classes
tracked_classes = {}
# Global variable that gets set to 'True' by the Main script
# when SConscript call tracing should be enabled.
sconscript_trace = False

def logInstanceCreation(instance, name=None) -> None:
    if name is None:
        name = instance.__class__.__name__
    if name not in tracked_classes:
        tracked_classes[name] = []
    if hasattr(instance, '__dict__'):
        tracked_classes[name].append(weakref.ref(instance))
    else:
        # weakref doesn't seem to work when the instance
        # contains only slots...
        tracked_classes[name].append(instance)

def string_to_classes(s):
    if s == '*':
        return sorted(tracked_classes.keys())
    else:
        return s.split()

def fetchLoggedInstances(classes: str="*"):
    classnames = string_to_classes(classes)
    return [(cn, len(tracked_classes[cn])) for cn in classnames]

def countLoggedInstances(classes, file=sys.stdout) -> None:
    for classname in string_to_classes(classes):
        file.write("%s: %d\n" % (classname, len(tracked_classes[classname])))

def listLoggedInstances(classes, file=sys.stdout) -> None:
    for classname in string_to_classes(classes):
        file.write('\n%s:\n' % classname)
        for ref in tracked_classes[classname]:
            if inspect.isclass(ref):
                obj = ref()
            else:
                obj = ref
            if obj is not None:
                file.write('    %s\n' % repr(obj))

def dumpLoggedInstances(classes, file=sys.stdout) -> None:
    for classname in string_to_classes(classes):
        file.write('\n%s:\n' % classname)
        for ref in tracked_classes[classname]:
            obj = ref()
            if obj is not None:
                file.write('    %s:\n' % obj)
                for key, value in obj.__dict__.items():
                    file.write('        %20s : %s\n' % (key, value))


if sys.platform[:5] == "linux":
    # Linux doesn't actually support memory usage stats from getrusage().
    def memory() -> int:
        with open('/proc/self/stat') as f:
            mstr = f.read()
        mstr = mstr.split()[22]
        return int(mstr)
elif sys.platform[:6] == 'darwin':
    #TODO really get memory stats for OS X
    def memory() -> int:
        return 0
elif sys.platform == 'win32':
    from SCons.compat.win32 import get_peak_memory_usage
    memory = get_peak_memory_usage
else:
    try:
        import resource
    except ImportError:
        def memory() -> int:
            return 0
    else:
        def memory() -> int:
            res = resource.getrusage(resource.RUSAGE_SELF)
            return res[4]


def caller_stack():
    """return caller's stack"""
    import traceback
    tb = traceback.extract_stack()
    # strip itself and the caller from the output
    tb = tb[:-2]
    result = []
    for back in tb:
        # (filename, line number, function name, text)
        key = back[:3]
        result.append('%s:%d(%s)' % func_shorten(key))
    return result

caller_bases = {}
caller_dicts = {}

def caller_trace(back: int=0) -> None:
    """
    Trace caller stack and save info into global dicts, which
    are printed automatically at the end of SCons execution.
    """
    global caller_bases, caller_dicts
    import traceback
    tb = traceback.extract_stack(limit=3+back)
    tb.reverse()
    callee = tb[1][:3]
    caller_bases[callee] = caller_bases.get(callee, 0) + 1
    for caller in tb[2:]:
        caller = callee + caller[:3]
        try:
            entry = caller_dicts[callee]
        except KeyError:
            caller_dicts[callee] = entry = {}
        entry[caller] = entry.get(caller, 0) + 1
        callee = caller

# print a single caller and its callers, if any
def _dump_one_caller(key, file, level: int=0) -> None:
    leader = '      '*level
    for v,c in sorted([(-v,c) for c,v in caller_dicts[key].items()]):
        file.write("%s  %6d %s:%d(%s)\n" % ((leader,-v) + func_shorten(c[-3:])))
        if c in caller_dicts:
            _dump_one_caller(c, file, level+1)

# print each call tree
def dump_caller_counts(file=sys.stdout) -> None:
    for k in sorted(caller_bases.keys()):
        file.write("Callers of %s:%d(%s), %d calls:\n"
                    % (func_shorten(k) + (caller_bases[k],)))
        _dump_one_caller(k, file)

shorten_list = [
    ( '/scons/SCons/',          1),
    ( '/src/engine/SCons/',     1),
    ( '/usr/lib/python',        0),
]

if os.sep != '/':
    shorten_list = [(t[0].replace('/', os.sep), t[1]) for t in shorten_list]

def func_shorten(func_tuple):
    f = func_tuple[0]
    for t in shorten_list:
        i = f.find(t[0])
        if i >= 0:
            if t[1]:
                i = i + len(t[0])
            return (f[i:],)+func_tuple[1:]
    return func_tuple


TraceFP = {}
if sys.platform == 'win32':
    TraceDefault = 'con'
else:
    TraceDefault = '/dev/tty'
TimeStampDefault = False
StartTime = time.perf_counter()
PreviousTime = StartTime

def Trace(msg, tracefile=None, mode: str='w', tstamp: bool=False) -> None:
    """Write a trace message.

    Write messages when debugging which do not interfere with stdout.
    Useful in tests, which monitor stdout and would break with
    unexpected output. Trace messages can go to the console (which is
    opened as a file), or to a disk file; the tracefile argument persists
    across calls unless overridden.

    Args:
        tracefile: file to write trace message to. If omitted,
          write to the previous trace file (default: console).
        mode: file open mode (default: 'w')
        tstamp: write relative timestamps with trace. Outputs time since
          scons was started, and time since last trace (default: False)

    """
    global TraceDefault
    global TimeStampDefault
    global PreviousTime

    def trace_cleanup(traceFP) -> None:
        traceFP.close()

    if tracefile is None:
        tracefile = TraceDefault
    else:
        TraceDefault = tracefile
    if not tstamp:
        tstamp = TimeStampDefault
    else:
        TimeStampDefault = tstamp
    try:
        fp = TraceFP[tracefile]
    except KeyError:
        try:
            fp = TraceFP[tracefile] = open(tracefile, mode)
            atexit.register(trace_cleanup, fp)
        except TypeError:
            # Assume we were passed an open file pointer.
            fp = tracefile
    if tstamp:
        now = time.perf_counter()
        fp.write('%8.4f %8.4f:  ' % (now - StartTime, now - PreviousTime))
        PreviousTime = now
    fp.write(msg)
    fp.flush()

# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
