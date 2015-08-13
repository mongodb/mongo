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

def main(args):
    parser = argparse.ArgumentParser()
    parser.add_argument("-f", "--file", dest="file", help="path to json file containing history data")
    parser.add_argument("--rev", dest="rev", help="revision to examine for regressions")
    parser.add_argument("--ndays", default=7, type=int, dest="ndays", help="Check against commit form n days ago.")
    parser.add_argument("--threshold", default=0.1, type=float, dest="threshold", help="Flag an error if throughput is more than 'threshold'x100 percent off")
    parser.add_argument("--reference", dest="reference", help="Reference commit to compare against. Should be a githash")
    args = parser.parse_args()
    j = get_json(args.file)
    h = History(j)
    testnames = h.testnames()
    failed = False

    for test in testnames:
        this_one = h.seriesAtRevision(test, args.rev)
        print "checking %s.." % (test)
        if not this_one:
            print "\tno data at this revision, skipping"
            continue

        #If the new build is 10% lower than the target (3.0 will be used as the baseline for 3.2 for instance), consider it regressed.
        previous = h.seriesItemsNBefore(test, args.rev, 1)
        if not previous:
            print "\tno previous data, skipping"
            continue
        previous = previous[0]
        daysprevious = h.seriesItemsNDaysBefore(test, args.rev,args.ndays)
        reference = h.seriesAtRevision(test, args.reference)
        if previous["max"] - this_one["max"] >= (args.threshold * previous["max"]):
            print "\tregression found: drop from %s (commit %s) to %s" % (previous["max"], previous["revision"][:5], this_one["max"])
            Failed = True
        else :  
            print "\tno regresion against previous. "
        if not daysprevious :
            print "\tno regresion against previous. No n day data"
        elif daysprevious["max"] - this_one["max"] >= (args.threshold * daysprevious["max"]):
            print "\tregression found over days: drop from %s (commit %s) to %s" % (daysprevious["max"], daysprevious["revision"][:5], this_one["max"])
            Failed = True
        else:
            print "\tno regression against n day data"
        if not reference :
            print "\tno No data for reference commit"
        elif reference["max"] - this_one["max"] >= (args.threshold * reference["max"]):
            print "\tregression found over reference commit: drop from %s (commit %s) to %s" % (reference["max"], reference["revision"][:5], this_one["max"])
            Failed = True
        else:
            print "\tno regression against reference commit"

    if failed:
        sys.exit(1)
    else:
        sys.exit(0)

def get_json(filename):
    jf = open(filename, 'r')
    json_obj = json.load(jf)
    return json_obj

class History(object):
    def __init__(self, jsonobj):
        self._raw = sorted(jsonobj, key=lambda d: d["order"])

    def testnames(self):
        return set(list(itertools.chain.from_iterable([[z["name"] for z in c["data"]["results"]] for c in self._raw])))

    def seriesAtRevision(self, testname, revision):
        s = self.series(testname)
        for result in s:
            if result["revision"] == revision:
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


    # I tried to do this in the form of this file. I feel like it's unneccessarily complicated right now. 
    def seriesItemsNDaysBefore(self, testname, revision, n):
        """
            Returns the items in the series under the given test name that 
            appear 'n' days prior to the specified revision.
        """
        results = {}
        # Date for this revision
        s = self.seriesAtRevision(testname, revision)
        if s==[] : 
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
                result["end"] = commit["data"]["end"]
                result["order"] = commit["order"]
                result["max"] = max(f["ops_per_sec"] for f in result["results"].values() if type(f) == type({}))
                yield result


class TestResult:
    def __init__(self, json):
        self._raw = json

    #def max(self):




if __name__ == '__main__': 
    main(sys.argv[1:])
