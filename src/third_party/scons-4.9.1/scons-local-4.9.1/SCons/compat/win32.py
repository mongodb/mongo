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
"""Logic to replicate parts of pywin32 SCons uses."""

__all__ = ['get_current_process', 'get_memory_info', 'get_memory_usage', 'get_peak_memory_usage']

import ctypes
from ctypes import wintypes

#
# From Activestate Recipe
# https://code.activestate.com/recipes/578513-get-memory-usage-of-windows-processes-using-getpro/
# MIT licensed
#
GetCurrentProcess = ctypes.windll.kernel32.GetCurrentProcess
GetCurrentProcess.argtypes = []
GetCurrentProcess.restype = wintypes.HANDLE

SIZE_T = ctypes.c_size_t


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ('cb', wintypes.DWORD),
        ('PageFaultCount', wintypes.DWORD),
        ('PeakWorkingSetSize', SIZE_T),
        ('WorkingSetSize', SIZE_T),
        ('QuotaPeakPagedPoolUsage', SIZE_T),
        ('QuotaPagedPoolUsage', SIZE_T),
        ('QuotaPeakNonPagedPoolUsage', SIZE_T),
        ('QuotaNonPagedPoolUsage', SIZE_T),
        ('PagefileUsage', SIZE_T),
        ('PeakPagefileUsage', SIZE_T),
        ('PrivateUsage', SIZE_T),
    ]


GetProcessMemoryInfo = ctypes.windll.psapi.GetProcessMemoryInfo
GetProcessMemoryInfo.argtypes = [
    wintypes.HANDLE,
    ctypes.POINTER(PROCESS_MEMORY_COUNTERS_EX),
    wintypes.DWORD,
]
GetProcessMemoryInfo.restype = wintypes.BOOL


def get_current_process():
    """Return handle to current process."""
    return GetCurrentProcess()


def get_memory_info(process=None):
    """Return Win32 process memory counters structure as a dict."""
    if process is None:
        process = get_current_process()
    counters = PROCESS_MEMORY_COUNTERS_EX()
    ret = GetProcessMemoryInfo(process, ctypes.byref(counters),
                               ctypes.sizeof(counters))
    if not ret:
        raise ctypes.WinError()
    info = {name: getattr(counters, name)
                for name, _ in counters._fields_}
    return info


def get_memory_usage(process=None):
    """Return this process's memory usage in bytes."""
    info = get_memory_info(process=process)
    return info['PrivateUsage']


def get_peak_memory_usage(process=None):
    """Return this process's memory usage in bytes."""
    info = get_memory_info(process=process)
    return info['PeakWorkingSetSize']


if __name__ == '__main__':
    import pprint

    pprint.pprint(get_memory_info())
