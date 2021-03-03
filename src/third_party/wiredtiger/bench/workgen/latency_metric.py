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

# latency_metric.py
# Print latency metrics for workgen runs that generate monitor.json
import json, sys
from datetime import datetime

# A 'safe' divide shown as a string.
def divide(a, b):
    if b == 0.0:
        if a == 0.0:
            return '0.000  (no entries)'
        else:
            return 'infinite  (divide by zero)'
    else:
        return '%.3f' % (float(a) / float(b))

def value_as_str(self):
    return '%.3f' % self.latency_average()

# A collection of statastics that are related to a specific condition
# during the run, for example during checkpoints or not during checkpoints.
class Digest:
    def __init__(self):
        self.entries = 0
        self.ops = 0
        self.lat = 0
        self.lat_99 = 0
        self.lat_99_raw = 0
        self.lat_max = 0
        self.secs = 0.0

    def entry(self, secs, ops, lat, lat_99, lat_max):
        self.secs += secs
        self.ops += ops
        self.lat += lat * ops
        self.lat_99_raw += lat_99
        self.lat_99 += lat_99 * ops
        if lat_max > self.lat_max:
            self.lat_max = lat_max
        self.entries += 1

    def time_secs(self):
        return self.secs

    def latency_99_raw_average(self):
        return float(self.lat_99_raw) / float(self.entries)

    def latency_max(self):
        return self.lat_max

    def latency_average(self):
        return float(self.lat) / float(self.ops)

    def dump(self, prefix):
        print(prefix + 'json entries: ' + str(self.entries))
        print(prefix + 'operations: ' + str(self.ops))
        print(prefix + 'total latency us: ' + str(self.lat))
        print(prefix + 'latency 99% us, weighted sum: ' + str(self.lat_99))
        print(prefix + 'latency 99% us, raw sum: ' + str(self.lat_99_raw))
        print(prefix + 'latency max us: ' + str(self.lat_max))
        print(prefix + 'elapsed secs: ' + str(self.secs))

class Metric:
    def __init__(self, name, desc):
        self.name = name
        self.desc = desc
        self.value = 0.0

    def set_value(self, value):
        self.value = value

# A set of latency metrics collected for a single run.
# A user will see the short name, and if the script is run with the '--raw'
# option, the elaborated description will also be shown.
class FileMetrics:
    def __init__(self, filename):
        self.filename = filename
        all = []

        # This is the average latency for all read operations.
        # Lower is better.
        self.latency_avg = m = Metric('Average latency reads us',
            'total latency of read ops divided by operations')
        all.append(m)

        # This is the maximum latency over all read operations.
        # Lower is better.
        self.latency_max = m = Metric('Max latency reads us',
            'maximum of all read op latencies')
        all.append(m)

        # This is the ratio of the two previously reported numbers, latency_max
        # and latency_avg.
        #
        # Lower is better (best is 1.0), it means more predictable response
        # times.
        self.ratio_max_avg = m = Metric('Max vs average latency',
            'ratio of maximum latency to average latency for read ops')
        all.append(m)

        # This is the ratio of 99% latency reads, checkpoint vs normal.
        # That is, for all 1-second intervals that occur during a checkpoint,
        # we take the 99 percentile latency for read operations, and average
        # them, weighted by how many read operations were performed in each
        # interval.  We do the same for all 1-second intervals that occur
        # outside of a checkpoint, and finally take the ratio between these
        # two numbers.  This is a more sophisticated measure of the smoothness
        # of overall response times, looking at all latencies, rather than
        # focusing on the one worst latency.
        #
        # Lower is better (best is 1.0), it means more predictable response
        # times.
        self.ratio_latency_99 = m = Metric('Checkpoint vs normal 99%',
            'ratio of the average of all 99% latency operations, ' +
            'during checkpoint times vs normal')
        all.append(m)

        # The proportion of time spent in a checkpoint
        self.proportion_checkpoint_time = m = Metric('Proportion of ckpt time',
            'the proportion of time doing checkpoints')
        all.append(m)
        self.all_metrics = all
        self.read_normal = None
        self.read_ckpt = None
        self.read_all = None

    def calculate(self):
        with open(self.filename) as f:
            lines = f.readlines()
            s = '{ "ts" : [' + ','.join(lines) + '] }'
            json_data = json.loads(s)
            self.calculate_using_json(json_data)

    def calculate_using_json(self, json_data):
        ckpt_in_progress = False
        ckpt_count = 0
        prev_dt = None
        # We digest the latency stats during 'normal' (non-checkpoint) times
        # and also during checkpoint times.
        self.read_normal = Digest()
        self.read_ckpt = Digest()
        self.read_all = Digest()
        for entry in json_data['ts']:
            time_s = entry['localTime']
            dt = datetime.strptime(time_s, '%Y-%m-%dT%H:%M:%S.%fZ')
            is_ckpt = entry['workgen']['checkpoint']['active'] > 0
            if not ckpt_in_progress and is_ckpt:
                ckpt_count += 1
            ckpt_in_progress = is_ckpt

            # In the first entry, we don't have an elapsed time, so
            # we'll ignore its data.
            if prev_dt != None:
                timediff = dt - prev_dt
                seconds = timediff.total_seconds()
                if seconds <= 0.0:
                    raise Exception('invalid time span between entries')
                if is_ckpt:
                    digest = self.read_ckpt
                else:
                    digest = self.read_normal
                rentry = entry['workgen']['read']
                ops = rentry['ops per sec'] * seconds
                lat_avg = rentry['average latency']
                lat_99 = rentry['99% latency']
                lat_max = rentry['max latency']
                digest.entry(seconds, ops, lat_avg, lat_99, lat_max)
                self.read_all.entry(seconds, ops, lat_avg, lat_99, lat_max)
            prev_dt = dt
        if self.read_all.time_secs() == 0.0:
            raise(Exception(self.filename +
                ': no entries, or no time elapsed'))
        if self.read_normal.entries == 0 or self.read_normal.ops == 0:
            raise(Exception(self.filename +
                ': no operations or entries during non-checkpoint time period'))
        if self.read_ckpt.entries == 0 or self.read_ckpt.ops == 0:
            raise(Exception(self.filename +
                ': no operations or entries during checkpoint'))
        if ckpt_count < 2:
            raise(Exception(self.filename +
                ': need at least 2 checkpoints started'))

        self.latency_avg.set_value(self.read_all.latency_average())
        self.latency_max.set_value(self.read_all.latency_max())
        self.ratio_max_avg.set_value(
            float(self.read_all.latency_max()) /
            float(self.read_all.latency_average()))
        self.ratio_latency_99.set_value(
            self.read_ckpt.latency_99_raw_average() /
            self.read_normal.latency_99_raw_average())
        self.proportion_checkpoint_time.set_value(
            self.read_ckpt.time_secs() /
            self.read_all.time_secs())

def table_line(leftcol, cols, spacer):
    return leftcol + spacer + spacer.join(cols) + '\n'

def make_len(value, l):
    return ('%%%ds' % l) % str(value)

def value_format(value):
    return '%.3f' % value

fmlist = []
raw = False
for arg in sys.argv[1:]:
    if arg == '--raw':
        raw = True
    else:
        fm = FileMetrics(arg)
        fm.calculate()
        fmlist.append(fm)

leftlen = 25
collen = 20
filecount = len(fmlist)
dashes = '-' * leftlen, []
if filecount == 0:
    print('Usage: python latency_metric.py [ --raw ] file.json...')
    print('  input files are typically monitor.json files produced by workgen')
else:
    out = ''
    cols = [make_len(fm.filename, collen) for fm in fmlist]
    out += table_line(' ' * leftlen, cols, ' | ')
    cols = ['-' * collen for fm in fmlist]
    out += table_line('-' * leftlen, cols, '-+-')
    pos = 0
    for m in fmlist[0].all_metrics:
        cols = [make_len(value_format(fm.all_metrics[pos].value), collen) \
            for fm in fmlist]
        out += table_line(make_len(m.name, leftlen), cols, ' | ')
        pos += 1

    print(out)

if raw:
    for fm in fmlist:
        print('file: ' + fm.filename)
        print('  digested metrics collected for reads during non-checkpoints:')
        fm.read_normal.dump('    ')
        print('  digested metrics collected for reads during checkpoints:')
        fm.read_ckpt.dump('    ')
        print('')
        for m in fm.all_metrics:
            print('  ' + m.name + ' (' + m.desc + '): ' + str(m.value))
        print('\nSee ' + __file__ +
            ' for a more detailed description of each metric.')
