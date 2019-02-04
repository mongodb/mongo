''' Output a doxgen version of the wtperf configuration options. '''
from __future__ import print_function
import sys

for line in sys.stdin:
    if not line.startswith('OPTION '):
        continue

    line = line.replace('OPTION ', '')
    v = line.split('",')
    v[0] = v[0].replace('"', '').strip()
    v[1] = v[1].replace('"', '').strip()
    v[2] = v[2].replace('"', '').strip()
    v[3] = v[3].replace('"', '').strip()

    if v[3] == 'boolean':
        if v[2] == '0':
            d = 'false'
        else:
            d = 'true'
    elif v[3] == 'string':
        d = '"' + v[2] + '"'
    else:
        d = v[2]
    print('@par ' + v[0] + ' (' + v[3] + ', default=' + d + ')')
    print(v[1])
