package command

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/util"
	"strconv"
)

// Struct implementing the Command interface for the serverStatus command.
type ServerStatus struct {
	Locks map[string]NSLocksInfo `bson:"locks"`
}

// Subfield of the serverStatus command.
type NSLocksInfo struct {
	TimeLockedMicros map[string]int `bson:"timeLockedMicros"`
}

// Implements the Diff interface for the diff between two serverStatus commands.
type ServerStatusDiff struct {
	// namespace - > totals
	Totals map[string][]int
}

// Implement the Diff interface.  Serializes the lock totals into rows by
// namespace.
func (self *ServerStatusDiff) ToRows() [][]string {
	// to return
	rows := [][]string{}

	// the header row
	headerRow := []string{"db", "total", "read", "write"}
	rows = append(rows, headerRow)

	// create the rows for the individual namespaces
	for ns, nsTotals := range self.Totals {

		nsRow := []string{ns}
		for _, total := range nsTotals {
			nsRow = append(nsRow, strconv.Itoa(util.MaxInt(0, total/1000))+"ms")
		}
		rows = append(rows, nsRow)

	}

	return rows
}

// Needed to implement the common/db/command's Command interface, in order to
// be run as a command against the database.
func (self *ServerStatus) AsRunnable() interface{} {
	return "serverStatus"
}

// Needed to implement the local package's Command interface. Diffs the server
// status result against another server status result.
func (self *ServerStatus) Diff(other Command) (Diff, error) {

	// the diff to eventually return
	diff := &ServerStatusDiff{
		Totals: map[string][]int{},
	}

	var otherAsServerStatus *ServerStatus
	var ok bool
	if otherAsServerStatus, ok = other.(*ServerStatus); !ok {
		return nil, fmt.Errorf("a *ServerStatus can only diff against another" +
			" *ServerStatus")
	}

	firstLocks := otherAsServerStatus.Locks
	secondLocks := self.Locks
	for ns, firstNSInfo := range firstLocks {
		if secondNSInfo, ok := secondLocks[ns]; ok {

			firstTimeLocked := firstNSInfo.TimeLockedMicros
			secondTimeLocked := secondNSInfo.TimeLockedMicros

			diff.Totals[ns] = []int{
				(secondTimeLocked["r"] + secondTimeLocked["R"]) -
					(firstTimeLocked["r"] + firstTimeLocked["R"]),
				(secondTimeLocked["w"] + secondTimeLocked["W"]) -
					(firstTimeLocked["w"] + firstTimeLocked["W"]),
			}

			diff.Totals[ns] = append(
				[]int{diff.Totals[ns][0] + diff.Totals[ns][1]},
				diff.Totals[ns]...,
			)
		}
	}

	return diff, nil
}
