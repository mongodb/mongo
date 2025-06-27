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

"""
SCons statistics routines.

This package provides a way to gather various statistics during an SCons
run and dump that info in several formats

Additionally, it probably makes sense to do stderr/stdout output of
those statistics here as well

There are basically two types of stats:

1. Timer (start/stop/time) for specific event.  These events can be
   hierarchical. So you can record the children events of some parent.
   Think program compile could contain the total Program builder time,
   which could include linking, and stripping the executable

2. Counter. Counting the number of events and/or objects created. This
   would likely only be reported at the end of a given SCons run,
   though it might be useful to query during a run.
"""

from abc import ABC

import platform
import json
import sys
from datetime import datetime

import SCons.Debug

all_stats = {}
ENABLE_JSON = False
JSON_OUTPUT_FILE = 'scons_stats.json'

def add_stat_type(name, stat_object):
    """Add a statistic type to the global collection"""
    if name in all_stats:
        raise UserWarning(f'Stat type {name} already exists')
    all_stats[name] = stat_object


class Stats(ABC):
    def __init__(self):
        self.stats = []
        self.labels = []
        self.append = self.do_nothing
        self.print_stats = self.do_nothing
        self.enabled = False

    def do_append(self, label):
        raise NotImplementedError

    def do_print(self):
        raise NotImplementedError

    def enable(self, outfp):
        self.outfp = outfp
        self.append = self.do_append
        self.print_stats = self.do_print
        self.enabled = True

    def do_nothing(self, *args, **kw):
        pass


class CountStats(Stats):

    def __init__(self):
        super().__init__()
        self.stats_table = {}

    def do_append(self, label):
        self.labels.append(label)
        self.stats.append(SCons.Debug.fetchLoggedInstances())

    def do_print(self):
        self.stats_table = {}
        for s in self.stats:
            for n in [t[0] for t in s]:
                self.stats_table[n] = [0, 0, 0, 0]
        i = 0
        for s in self.stats:
            for n, c in s:
                self.stats_table[n][i] = c
            i = i + 1
        self.outfp.write("Object counts:\n")
        pre = ["   "]
        post = ["   %s\n"]
        l = len(self.stats)
        fmt1 = ''.join(pre + [' %7s'] * l + post)
        fmt2 = ''.join(pre + [' %7d'] * l + post)
        labels = self.labels[:l]
        labels.append(("", "Class"))
        self.outfp.write(fmt1 % tuple(x[0] for x in labels))
        self.outfp.write(fmt1 % tuple(x[1] for x in labels))
        for k in sorted(self.stats_table.keys()):
            r = self.stats_table[k][:l] + [k]
            self.outfp.write(fmt2 % tuple(r))


class MemStats(Stats):
    def do_append(self, label):
        self.labels.append(label)
        self.stats.append(SCons.Debug.memory())

    def do_print(self):
        fmt = 'Memory %-32s %12d\n'
        for label, stats in zip(self.labels, self.stats):
            self.outfp.write(fmt % (label, stats))


class TimeStats(Stats):
    def __init__(self):
        super().__init__()
        self.totals = {}
        self.commands = {}  # we get order from insertion order, and can address individual via dict

    def total_times(self, build_time, sconscript_time, scons_exec_time, command_exec_time):
        self.totals = {
            'build_time': build_time,
            'sconscript_time': sconscript_time,
            'scons_exec_time': scons_exec_time,
            'command_exec_time': command_exec_time
        }

    def add_command(self, command, start_time, finish_time):
        if command in self.commands:
            print("Duplicate command %s" % command)
        self.commands[command] = {'start': start_time,
                                  'end' : finish_time,
                                  'duration': finish_time - start_time}


count_stats = CountStats()
memory_stats = MemStats()
time_stats = TimeStats()


def write_scons_stats_file():
    """
    Actually write the JSON file with debug information.
    Depending which of : count, time, action-timestamps,memory their information will be written.
    """

    # Have to import where used to avoid import loop
    from SCons.Script import (  # pylint: disable=import-outside-toplevel
        BUILD_TARGETS,
        COMMAND_LINE_TARGETS,
        ARGUMENTS,
        ARGLIST,
    )
    # print(f"DUMPING JSON FILE: {JSON_OUTPUT_FILE}")
    json_structure = {}
    if count_stats.enabled:
        json_structure['Object counts'] = {}

        oc = json_structure['Object counts']
        for c in count_stats.stats_table:
            oc[c] = {}
            for l, v in zip(count_stats.labels, count_stats.stats_table[c]):
                oc[c][''.join(l)] = v

    if memory_stats.enabled:
        json_structure['Memory'] = {}

        m = json_structure['Memory']
        for label, stats in zip(memory_stats.labels, memory_stats.stats):
            m[label] = stats

    if time_stats.enabled:
        json_structure['Time'] = {'Commands': time_stats.commands,
                                  'Totals': time_stats.totals}

    # Now add information about this build to the JSON file
    json_structure['Build_Info'] = {
        'BUILD_TARGETS' : [str(t) for t in BUILD_TARGETS],
        'ARGUMENTS' : [str(a) for a in ARGUMENTS],
        'ARGLIST' : [ str(al) for al in ARGLIST],
        'COMMAND_LINE_TARGETS' : [ str(clt) for clt in COMMAND_LINE_TARGETS],
        'ARGV' : sys.argv,
        'TIME' : datetime.now().isoformat(),
        'HOST' : platform.node(),
        'PYTHON_VERSION' : {
            'major' : sys.version_info.major,
            'minor' : sys.version_info.minor,
            'micro' : sys.version_info.micro,
            'releaselevel' : sys.version_info.releaselevel,
            'serial' : sys.version_info.serial,
        }
    }


    with open(JSON_OUTPUT_FILE, 'w') as sf:
        sf.write(json.dumps(json_structure, indent=4))


# Local Variables:
# tab-width:4
# indent-tabs-mode:nil
# End:
# vim: set expandtab tabstop=4 shiftwidth=4:
