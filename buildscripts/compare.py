import json
import sys
import argparse


parser = argparse.ArgumentParser()
parser.add_argument("-b", "--baseline", default="base.json", dest="baseline",
                    help="path to json file containing baseline data")
parser.add_argument("-c", "--comparison", default="compare.json", dest="compare",
                    help="path to json file containing comparison data")
parser.add_argument("-t", "--threshold", default=75, dest="threshold", 
                    help="Comparison threshold in percent. Ex. -t 75 will fail if\n"
                    "the comparison is less than 75% of the reference value")
args = parser.parse_args()

compare = json.load(open(args.compare))
baseline = json.load(open(args.baseline))
baselinedict = dict((s['name'], s) for s in baseline['results'])

threshold = float(args.threshold)

# Note, we're putting things in an ops_per_sec fields, but it's really
# a ratio. Would like to rename and have evergreen pick it up.

newresults = []
reportresults = []
fails = []
for result in compare['results'] :
    nresult = {'name' : result['name']}
    nreport = {'test_file' : result['name'], 'exit_code' : 0, 'elapsed' : 5,
               'start': 1441227291.962453, 'end': 1441227293.428761}
    r = result['results']
    s = baselinedict[result['name']]['results']
    nresult['results'] =  dict((thread,
                                {'ops_per_sec' : 100*r[thread]['ops_per_sec']/s[thread]['ops_per_sec']})
                               for thread in r if type(r[thread]) == type({}) and thread in s)
    newresults.append(nresult)
    failingThreads = [thread for thread in nresult['results']
                      if nresult['results'][thread]['ops_per_sec'] < threshold]
    if len(failingThreads) > 0 :
        nreport['status'] = 'fail'
        for thread in failingThreads:
            print "Test %s failed comparison to baseline for thread level %s. Achieved %.2f %%" \
            " of the performance of baseline." % \
            (result['name'], thread, nresult['results'][thread]['ops_per_sec'])
        fails.append((result['name'], failingThreads))
    else:
        nreport['status'] = 'pass'
    reportresults.append(nreport)

out = open("perf.json", 'w')
json.dump({'results' : newresults}, out, indent=4, separators=(',', ':'))

report = {}
report['failures'] = len(fails)
report['results'] = reportresults

reportFile = open('report.json', 'w')
json.dump(report, reportFile, indent=4, separators=(',', ': '))

if len(fails) > 0: 
    print "There were failing tests"
    print fails
    sys.exit(1)
else:
    sys.exit(0)
