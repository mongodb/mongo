package result

import (
	"github.com/shelman/mongo-tools-proto/common/util"
)

// Struct for the output of the top command run against a mongo database.
type TopResults struct {
	// namespace -> namespace-specific top info
	Totals map[string]NSTopInfo `bson:"totals"`
}

// Info within the top command about a single namespace.
type NSTopInfo struct {
	Total TopField `bson:"total"`
	Read  TopField `bson:"readLock"`
	Write TopField `bson:"writeLock"`
}

// Top information about a single field in a namespace.
type TopField struct {
	Time  int `bson:"time"`
	Count int `bson:"count"`
}

// Diff two top results, returning another top result containing the
// differences.
func Diff(first, second *TopResults) *TopResults {

	// the diff to eventually return
	diff := &TopResults{
		Totals: map[string]NSTopInfo{},
	}

	// create the fields for each namespace existing in both the old and
	// new results
	firstTotals := first.Totals
	secondTotals := second.Totals
	for ns, firstNSInfo := range firstTotals {
		if secondNSInfo, ok := secondTotals[ns]; ok {
			diff.Totals[ns] = NSTopInfo{
				Total: TopField{
					Time: util.MaxInt(
						0, secondNSInfo.Total.Time-firstNSInfo.Total.Time,
					),
					Count: util.MaxInt(
						0, secondNSInfo.Total.Count-firstNSInfo.Total.Count,
					),
				},
				Read: TopField{
					Time: util.MaxInt(
						0, secondNSInfo.Read.Time-firstNSInfo.Read.Time,
					),
					Count: util.MaxInt(
						0, secondNSInfo.Read.Count-firstNSInfo.Read.Count,
					),
				},
				Write: TopField{
					Time: util.MaxInt(
						0, secondNSInfo.Write.Time-firstNSInfo.Write.Time,
					),
					Count: util.MaxInt(
						0, secondNSInfo.Write.Count-firstNSInfo.Write.Count,
					),
				},
			}
		}
	}

	return diff
}
