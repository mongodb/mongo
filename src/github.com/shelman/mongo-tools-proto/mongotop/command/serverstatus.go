package command

import (
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/util"
	"strconv"
)

type ServerStatus struct {
	Locks map[string]NSLocksInfo `bson:"locks"`
}

type NSLocksInfo struct {
	TimeLockedMicros map[string]int `bson:"timeLockedMicros"`
}

type ServerStatusDiff struct {
	// namespace - > totals
	Totals map[string][]int
}

// implement dat interface
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

// implement dat interface
func (self *ServerStatus) AsRunnable() interface{} {
	return "serverStatus"
}

// implement dat other interface
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
