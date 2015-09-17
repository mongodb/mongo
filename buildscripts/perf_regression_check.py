import argparse
import json
import sys
import itertools
from dateutil import parser
from datetime import timedelta, datetime

# Example usage:
# perf_regression_check.py -f history_file.json --rev 18808cd923789a34abd7f13d62e7a73fafd5ce5f
# Loads the history json file, and looks for regressions at the revision 18808cd...
# Will exit with status code 1 if any regression is found, 0 otherwise.

def compareOneResultNoise(this_one, reference, label, threadlevel="max", noiseLevel=0,
                          noiseMultiple=1, minThreshold=0.05):
    '''
    Take two result series and compare them to see if they are acceptable.
    Return true if failed, and false if pass
    Uses historical noise data for the comparison.

    '''
    failed = False;
    if not reference:
        return failed

    ref = ""
    current = ""
    noise = 0

    if threadlevel == "max":
        ref = reference["max"]
        current = this_one["max"]
    else:
        # Don't do a comparison if the thread data is missing
        if not threadlevel in reference["results"].keys():
            return failed
        ref = reference["results"][threadlevel]['ops_per_sec']
        current = this_one["results"][threadlevel]['ops_per_sec']

    noise = noiseLevel * noiseMultiple
    delta = minThreshold * ref
    if (delta < noise):
        delta = noise
    # Do the check
    if ref - current >= delta:
        print ("\tregression found on %s: drop from %.2f ops/sec (commit %s) to %.2f ops/sec for comparison %s. Diff is"
               " %.2f ops/sec (%.2f%%), noise level is %.2f ops/sec and multiple is %.2f" %
               (threadlevel, ref, reference["revision"][:5], current, label, ref - current,
                100*(ref-current)/ref, noiseLevel, noiseMultiple))
        failed = True
    return failed


def compareResults(this_one, reference, threshold, label, noiseLevels={}, noiseMultiple=1, threadThreshold=None, threadNoiseMultiple=None):
    '''
    Take two result series and compare them to see if they are acceptable.
    Return true if failed, and false if pass
    '''

    failed = False;
    if not reference:
        return failed
    # Default threadThreshold to the same as the max threshold
    if  not threadThreshold:
        threadThreshold = threshold
    if not threadNoiseMultiple : 
        threadNoiseMultiple = noiseMultiple

    # Check max throughput first
    noise = 0
    # For the max throughput, use the max noise across the thread levels as the noise parameter
    if len(noiseLevels.values()) > 0:
        noise = max(noiseLevels.values())
    if compareOneResultNoise(this_one, reference, label, "max", noiseLevel=noise,
                             noiseMultiple=noiseMultiple, minThreshold=threshold):
        failed = True;
    # Check for regression on threading levels
    for (level, ops_per_sec) in (((r, this_one["results"][r]['ops_per_sec']) for r in
                                  this_one["results"] if type(this_one["results"][r]) == type({}))):
        noise = 0
        if level in noiseLevels:
            noise = noiseLevels[level]
        if compareOneResultNoise(this_one, reference, label, level, noiseLevel=noise,
                                 noiseMultiple=threadNoiseMultiple, minThreshold=threadThreshold):
            failed = True
    if not failed:
        print "\tno regression against %s and githash %s" %(label, reference["revision"][:5])
    return failed



def main(args):
    parser = argparse.ArgumentParser()
    parser.add_argument("-f", "--file", dest="file", help="path to json file containing"
                        "history data")
    parser.add_argument("-t", "--tagFile", dest="tfile", help="path to json file containing"
                        "tag data")
    parser.add_argument("--rev", dest="rev", help="revision to examine for regressions")
    parser.add_argument("--ndays", default=7, type=int, dest="ndays", help="Check against"
                        "commit from n days ago.")
    parser.add_argument("--threshold", default=0.05, type=float, dest="threshold", help=
                        "Don't flag an error if throughput is less than 'threshold'x100 percent off")
    parser.add_argument("--noiseLevel", default=1, type=float, dest="noise", help=
                        "Don't flag an error if throughput is less than 'noise' times the computed noise level off")
    parser.add_argument("--threadThreshold", default=0.1, type=float, dest="threadThreshold", help=
                        "Don't flag an error if thread level throughput is more than"
                        "'threadThreshold'x100 percent off")
    parser.add_argument("--threadNoiseLevel", default=2, type=float, dest="threadNoise", help=
                        "Don't flag an error if thread level throughput is less than 'noise' times the computed noise level off")
    parser.add_argument("--refTag", dest="reference", help=
                        "Reference tag to compare against. Should be a valid tag name")
    parser.add_argument("--overrideFile", dest="overrideFile", help="File to read for comparison override information")
    parser.add_argument("--variant", dest="variant", help="Variant to lookup in the override file")

    args = parser.parse_args()
    tagHistory = ""
    j = get_json(args.file)
    if args.tfile : 
        t = get_json(args.tfile)
        tagHistory = History(t)
    history = History(j)
    testnames = history.testnames()
    failed = False
    failed = 0

    results = []
    # Default empty override structure
    overrides = {'ndays' : {}, 'reference' : {}}
    if args.overrideFile :
        # Read the overrides file
        foverrides = get_json(args.overrideFile)
        # Is this variant in the overrides file?
        if args.variant in foverrides : 
            overrides = foverrides[args.variant]

    for test in testnames:
        # The first entry is valid. The rest is dummy data to match the existing format
        result = {'test_file' : test, 'exit_code' : 0, 'elapsed' : 5, 'start': 1441227291.962453, 'end': 1441227293.428761}
        this_one = history.seriesAtRevision(test, args.rev)
        testFailed = False
        print "checking %s.." % (test)
        if not this_one:
            print "\tno data at this revision, skipping"
            continue

        #If the new build is 10% lower than the target (3.0 will be
        #used as the baseline for 3.2 for instance), consider it
        #regressed.
        previous = history.seriesItemsNBefore(test, args.rev, 1)
        if not previous:
            print "\tno previous data, skipping"
            continue
        if compareResults(this_one, previous[0], args.threshold, "Previous", history.noiseLevels(test),
                          args.noise, args.threadThreshold, args.threadNoise):
            testFailed = True
            result['PreviousCompare'] = 'fail'
        else :
            result['PreviousCompare'] = 'pass'

        daysprevious = history.seriesItemsNDaysBefore(test, args.rev,args.ndays)
        if test in overrides['ndays']:
            print "Override in ndays for test %s" % test
            daysprevious = overrides['ndays'][test]
        if compareResults(this_one, daysprevious, args.threshold, "NDays", history.noiseLevels(test),
                          args.noise, args.threadThreshold, args.threadNoise):
            testFailed = True
            result['NDayCompare'] = 'fail'
        else :
            result['NDayCompare'] = 'pass'
        if tagHistory : 
            reference = tagHistory.seriesAtTag(test, args.reference)
            if not reference : 
                print "Didn't get any data for test %s with baseline %s" % (test, args.reference)
            if test in overrides['reference']:
                print "Override in references for test %s" % test
                reference = overrides['reference'][test]
            if compareResults(this_one, reference, args.threshold, "Baseline Comparison " + args.reference, history.noiseLevels(test),
                              args.noise, args.threadThreshold, args.threadNoise):
                testFailed = True
                result['BaselineCompare'] = 'fail'
            else :
                result['BaselineCompare'] = 'pass'
        if testFailed :
            result['status'] = 'fail'
            failed += 1
        else :
            result['status'] = 'pass'
        results.append(result)
        
    report = {}
    report['failures'] = failed
    report['results'] = results

    reportFile = open('report.json', 'w')
    json.dump(report, reportFile, indent=4, separators=(',', ': '))
    if failed > 0 :
        sys.exit(1)
    else:
        sys.exit(0)

# We wouldn't need this function if we had numpy installed on the system
def computeRange(result_list):
    '''
       Compute the max, min, and range (max - min) for the result list
    '''
    min = max = result_list[0]
    for result in result_list:
        if result < min:
            min = result
        if result > max:
            max = result
    return (max,min,max-min)

def get_json(filename):
    jf = open(filename, 'r')
    json_obj = json.load(jf)
    return json_obj

class History(object):
    def __init__(self, jsonobj):
        self._raw = sorted(jsonobj, key=lambda d: d["order"])
        self._noise = None

    def testnames(self):
        return set(list(itertools.chain.from_iterable([[z["name"] for z in c["data"]["results"]]
                                                       for c in self._raw])))

    def seriesAtRevision(self, testname, revision):
        s = self.series(testname)
        for result in s:
            if result["revision"] == revision:
                return result
        return None

    def seriesAtTag(self, testname, tagName):
        s = self.series(testname)
        for result in s:
            if result["tag"] == tagName:
                return result
        return None

    def seriesItemsNBefore(self, testname, revision, n):
        """
            Returns the 'n' items in the series under the given test name that
            appear prior to the specified revision.
        """
        results = []
        found = False
        s = self.series(testname)
        for result in s:
            if result["revision"] == revision:
                found = True
                break
            results.append(result)

        if found:
            return results[-1*n:]
        return []

    def computeNoiseLevels(self):
        """
        For each test, go through all results, and compute the average
        noise (max - min) for the series

        """
        self._noise = {}
        testnames = self.testnames()
        for test in testnames:
            self._noise[test] = {}
            s = self.series(test)
            threads = []
            for result in s:
                threads = result["threads"]
                break

            # Determine levels from last commit? Probably a better way to do this.
            for thread in threads:
                s = self.series(test)
                self._noise[test][thread] = sum((computeRange(x["results"][thread]["ops_per_sec_values"])[2]
                                                 for x in s))
                s = self.series(test)
                self._noise[test][thread] /= sum(1 for x in s)


    def noiseLevels(self, testname):
        """
        Returns the average noise level of the given test. Noise levels
        are thread specific. Returns an array

        """
        # check if noise has been computed. Compute if it hasn't
        if not self._noise:
            print "Computing noise levels"
            self.computeNoiseLevels()
        # Look up noise value for test
        if not testname in self._noise:
            print "Test %s not in self._noise" % (testname)
        return self._noise[testname]


    def seriesItemsNDaysBefore(self, testname, revision, n):
        """
            Returns the items in the series under the given test name that
            appear 'n' days prior to the specified revision.
        """
        results = {}
        # Date for this revision
        s = self.seriesAtRevision(testname, revision)
        if s==[]:
            return []
        refdate = parser.parse(s["end"]) - timedelta(days=n)

        s = self.series(testname)
        for result in s:
            if parser.parse(result["end"]) < refdate:
                results = result
        return results



    def series(self, testname):
        for commit in self._raw:
            # get a copy of the samples for those whose name matches the given testname
            matching = filter( lambda x: x["name"]==testname, commit["data"]["results"])
            if matching:
                result = matching[0]
                result["revision"] = commit["revision"]
                result["tag"] = commit["tag"]
                result["end"] = commit["data"]["end"]
                result["order"] = commit["order"]
                result["max"] = max(f["ops_per_sec"] for f in result["results"].values()
                                    if type(f) == type({}))
                result["threads"] = [f for f in result["results"] if type(result["results"][f])
                                     == type({})]
                yield result


class TestResult:
    def __init__(self, json):
        self._raw = json

    #def max(self):

if __name__ == '__main__':
    main(sys.argv[1:])
