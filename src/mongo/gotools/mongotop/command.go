// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongotop

import (
	"bytes"
	"encoding/json"
	"fmt"
	"github.com/mongodb/mongo-tools/common/text"
	"sort"
	"time"
)

// FormattableDiff represents a diff of two samples taken by mongotop,
// which can be printed to output in various formats.
type FormattableDiff interface {
	// Generate a JSON representation of the diff
	JSON() string
	// Generate a table-like representation which can be printed to a terminal
	Grid() string
}

// ServerStatus represents the results of the "serverStatus" command.
type ServerStatus struct {
	Locks map[string]LockStats `bson:"locks,omitempty"`
}

// LockStats contains information on time spent acquiring and holding a lock.
type LockStats struct {
	AcquireCount        *ReadWriteLockTimes `bson:"acquireCount"`
	TimeLockedMicros    ReadWriteLockTimes  `bson:"timeLockedMicros"`
	TimeAcquiringMicros ReadWriteLockTimes  `bson:"timeAcquiringMicros"`
}

// ReadWriteLockTimes contains read/write lock times on a database.
type ReadWriteLockTimes struct {
	Read       int64 `bson:"R"`
	Write      int64 `bson:"W"`
	ReadLower  int64 `bson:"r"`
	WriteLower int64 `bson:"w"`
}

// ServerStatusDiff contains a map of the lock time differences for each database.
type ServerStatusDiff struct {
	// namespace -> lock times
	Totals map[string]LockDelta `json:"totals"`
	Time   time.Time            `json:"time"`
}

// LockDelta represents the differences in read/write lock times between two samples.
type LockDelta struct {
	Read  int64 `json:"read"`
	Write int64 `json:"write"`
}

// TopDiff contains a map of the differences between top samples for each namespace.
type TopDiff struct {
	// namespace -> totals
	Totals map[string]NSTopInfo `json:"totals"`
	Time   time.Time            `json:"time"`
}

// Top holds raw output of the "top" command.
type Top struct {
	Totals map[string]NSTopInfo `bson:"totals"`
}

// NSTopInfo holds information about a single namespace.
type NSTopInfo struct {
	Total TopField `bson:"total" json:"total"`
	Read  TopField `bson:"readLock" json:"read"`
	Write TopField `bson:"writeLock" json:"write"`
}

// TopField contains the timing and counts for a single lock statistic within the "top" command.
type TopField struct {
	Time  int `bson:"time" json:"time"`
	Count int `bson:"count" json:"count"`
}

// struct to enable sorting of namespaces by lock time with the sort package
type sortableTotal struct {
	Name  string
	Total int64
}

type sortableTotals []sortableTotal

func (a sortableTotals) Less(i, j int) bool {
	if a[i].Total == a[j].Total {
		return a[i].Name > a[j].Name
	}
	return a[i].Total < a[j].Total
}
func (a sortableTotals) Len() int      { return len(a) }
func (a sortableTotals) Swap(i, j int) { a[i], a[j] = a[j], a[i] }

// Diff takes an older Top sample, and produces a TopDiff
// representing the deltas of each metric between the two samples.
func (top Top) Diff(previous Top) TopDiff {
	// The diff to eventually return
	diff := TopDiff{
		Totals: map[string]NSTopInfo{},
		Time:   time.Now(),
	}

	// For each namespace we are tracking, subtract the times and counts
	// for total/read/write and build a new map containing the diffs.
	prevTotals := previous.Totals
	curTotals := top.Totals
	for ns, prevNSInfo := range prevTotals {
		if curNSInfo, ok := curTotals[ns]; ok {
			diff.Totals[ns] = NSTopInfo{
				Total: TopField{
					Time:  (curNSInfo.Total.Time - prevNSInfo.Total.Time) / 1000,
					Count: curNSInfo.Total.Count - prevNSInfo.Total.Count,
				},
				Read: TopField{
					Time:  (curNSInfo.Read.Time - prevNSInfo.Read.Time) / 1000,
					Count: curNSInfo.Read.Count - prevNSInfo.Read.Count,
				},
				Write: TopField{
					Time:  (curNSInfo.Write.Time - prevNSInfo.Write.Time) / 1000,
					Count: curNSInfo.Write.Count - prevNSInfo.Write.Count,
				},
			}
		}
	}
	return diff
}

// Grid returns a tabular representation of the TopDiff.
func (td TopDiff) Grid() string {
	buf := &bytes.Buffer{}
	out := &text.GridWriter{ColumnPadding: 4}
	out.WriteCells("ns", "total", "read", "write", time.Now().Format("2006-01-02T15:04:05Z07:00"))
	out.EndRow()

	//Sort by total time
	totals := make(sortableTotals, 0, len(td.Totals))
	for ns, diff := range td.Totals {
		totals = append(totals, sortableTotal{ns, int64(diff.Total.Time)})
	}

	sort.Sort(sort.Reverse(totals))
	for i, st := range totals {
		diff := td.Totals[st.Name]
		out.WriteCells(st.Name,
			fmt.Sprintf("%vms", diff.Total.Time),
			fmt.Sprintf("%vms", diff.Read.Time),
			fmt.Sprintf("%vms", diff.Write.Time),
			"")
		out.EndRow()
		if i >= 9 {
			break
		}
	}
	out.Flush(buf)
	return buf.String()
}

// JSON returns a JSON representation of the TopDiff.
func (td TopDiff) JSON() string {
	bytes, err := json.Marshal(td)
	if err != nil {
		panic(err)
	}
	return string(bytes)
}

// JSON returns a JSON representation of the ServerStatusDiff.
func (ssd ServerStatusDiff) JSON() string {
	bytes, err := json.Marshal(ssd)
	if err != nil {
		panic(err)
	}
	return string(bytes)
}

// Grid returns a tabular representation of the ServerStatusDiff.
func (ssd ServerStatusDiff) Grid() string {
	buf := &bytes.Buffer{}
	out := &text.GridWriter{ColumnPadding: 4}
	out.WriteCells("db", "total", "read", "write", time.Now().Format("2006-01-02T15:04:05Z07:00"))
	out.EndRow()

	//Sort by total time
	totals := make(sortableTotals, 0, len(ssd.Totals))
	for ns, diff := range ssd.Totals {
		totals = append(totals, sortableTotal{ns, diff.Read + diff.Write})
	}

	sort.Sort(sort.Reverse(totals))
	for i, st := range totals {
		diff := ssd.Totals[st.Name]
		out.WriteCells(st.Name,
			fmt.Sprintf("%vms", diff.Read+diff.Write),
			fmt.Sprintf("%vms", diff.Read),
			fmt.Sprintf("%vms", diff.Write),
			"")
		out.EndRow()
		if i >= 9 {
			break
		}
	}

	out.Flush(buf)
	return buf.String()
}

// Diff takes an older ServerStatus sample, and produces a ServerStatusDiff
// representing the deltas of each metric between the two samples.
func (ss ServerStatus) Diff(previous ServerStatus) ServerStatusDiff {
	// the diff to eventually return
	diff := ServerStatusDiff{
		Totals: map[string]LockDelta{},
		Time:   time.Now(),
	}

	prevLocks := previous.Locks
	curLocks := ss.Locks
	for ns, prevNSInfo := range prevLocks {
		if curNSInfo, ok := curLocks[ns]; ok {
			prevTimeLocked := prevNSInfo.TimeLockedMicros
			curTimeLocked := curNSInfo.TimeLockedMicros

			diff.Totals[ns] = LockDelta{
				Read: (curTimeLocked.Read + curTimeLocked.ReadLower -
					(prevTimeLocked.Read + prevTimeLocked.ReadLower)) / 1000,
				Write: (curTimeLocked.Write + curTimeLocked.WriteLower -
					(prevTimeLocked.Write + prevTimeLocked.WriteLower)) / 1000,
			}
		}
	}

	return diff
}
