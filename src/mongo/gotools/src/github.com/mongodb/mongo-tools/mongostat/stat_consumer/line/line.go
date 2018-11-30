// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package line

import (
	"github.com/mongodb/mongo-tools/mongostat/status"
)

// StatLine is a wrapper for all metrics reported by mongostat for monitored hosts
type StatLine struct {
	Fields  map[string]string
	Error   error
	Printed bool
}

type StatLines []*StatLine

func (slice StatLines) Len() int {
	return len(slice)
}

func (slice StatLines) Less(i, j int) bool {
	return slice[i].Fields["host"] < slice[j].Fields["host"]
}

func (slice StatLines) Swap(i, j int) {
	slice[i], slice[j] = slice[j], slice[i]
}

// NewStatLine constructs a StatLine object from two ServerStatus objects
func NewStatLine(oldStat, newStat *status.ServerStatus, headerKeys []string, c *status.ReaderConfig) *StatLine {
	line := &StatLine{
		Fields: make(map[string]string),
	}
	for _, key := range headerKeys {
		_, ok := StatHeaders[key]
		if ok {
			line.Fields[key] = StatHeaders[key].ReadField(c, newStat, oldStat)
		} else {
			line.Fields[key] = status.InterpretField(key, newStat, oldStat)
		}
	}
	// We always need host and storage_engine, even if they aren't being displayed
	line.Fields["host"] = StatHeaders["host"].ReadField(c, newStat, oldStat)
	line.Fields["storage_engine"] = StatHeaders["storage_engine"].ReadField(c, newStat, oldStat)
	return line
}
