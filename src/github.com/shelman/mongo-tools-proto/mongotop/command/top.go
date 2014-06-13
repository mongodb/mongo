package command

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/util"
	"sort"
	"strconv"
	"strings"
	"time"
)

type Top struct {
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

type TopDiff struct {
	// namespace -> totals
	Totals map[string][]int
}

// implement dat interface
func (self *TopDiff) ToRows() [][]string {
	// to return
	rows := [][]string{}

	// the header row
	headerRow := []string{"ns", "total", "read", "write",
		time.Now().Format("2006-01-02T15:04:05")}
	rows = append(rows, headerRow)

	// sort the namespaces
	nsSorted := []string{}
	for ns, _ := range self.Totals {
		nsSorted = append(nsSorted, ns)
	}
	sort.Strings(nsSorted)

	// create the rows for the individual namespaces, iterating in sorted
	// order
	for _, ns := range nsSorted {

		nsTotals := self.Totals[ns]

		// some namespaces are skipped
		if skipNamespace(ns) {
			continue
		}

		nsRow := []string{ns}
		for _, total := range nsTotals {
			nsRow = append(nsRow, strconv.Itoa(util.MaxInt(0, total/1000))+"ms")
		}
		rows = append(rows, nsRow)
	}

	return rows
}

// skip dat namespace
func skipNamespace(ns string) bool {
	return ns == "" ||
		!strings.Contains(ns, ".") ||
		strings.HasSuffix(ns, "namespaces") ||
		strings.HasPrefix(ns, "local")
}

// Implement the common/db/command package's Command interface, in order to be
// run as a database command.
func (self *Top) AsRunnable() interface{} {
	return "top"
}

// Implement the local package's Command interface, for the purposes of
// computing and outputting diffs.
func (self *Top) Diff(other Command) (Diff, error) {

	// the diff to eventually return
	diff := &TopDiff{
		Totals: map[string][]int{},
	}

	// make sure the other command to be diffed against is of the same type
	var otherAsTop *Top
	var ok bool
	if otherAsTop, ok = other.(*Top); !ok {
		return nil, fmt.Errorf("a *Top can only diff against another *Top")
	}

	// create the fields for each namespace existing in both the old and
	// new results
	firstTotals := otherAsTop.Totals
	secondTotals := self.Totals
	for ns, firstNSInfo := range firstTotals {
		if secondNSInfo, ok := secondTotals[ns]; ok {
			diff.Totals[ns] = []int{
				secondNSInfo.Total.Time - firstNSInfo.Total.Time,
				secondNSInfo.Read.Time - firstNSInfo.Total.Time,
				secondNSInfo.Write.Time - firstNSInfo.Total.Time,
			}
		}
	}

	return diff, nil
}
