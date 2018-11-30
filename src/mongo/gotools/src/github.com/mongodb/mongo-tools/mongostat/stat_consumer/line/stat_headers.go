// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package line

import (
	"github.com/mongodb/mongo-tools/mongostat/status"
)

// Flags to determine cases when to activate/deactivate columns for output.
const (
	FlagAlways   = 1 << iota // always activate the column
	FlagHosts                // only active if we may have multiple hosts
	FlagDiscover             // only active when mongostat is in discover mode
	FlagRepl                 // only active if one of the nodes being monitored is in a replset
	FlagLocks                // only active if node is capable of calculating lock info
	FlagAll                  // only active if mongostat was run with --all option
	FlagMMAP                 // only active if node has mmap-specific fields
	FlagWT                   // only active if node has wiredtiger-specific fields
)

// StatHeader describes a single column for mongostat's terminal output,
// its formatting, and in which modes it should be displayed.
type StatHeader struct {
	// ReadField produces a particular field according to the StatHeader instance.
	// Some fields are based on a diff, so both latest ServerStatuses are taken.
	ReadField func(c *status.ReaderConfig, newStat, oldStat *status.ServerStatus) string
}

// StatHeaders are the complete set of data metrics supported by mongostat.
var (
	keyNames = map[string][]string{ // short, long, deprecated
		"host":           {"host", "Host", "host"},
		"storage_engine": {"storage_engine", "Storage engine", "engine"},
		"insert":         {"insert", "Insert opcounter (diff)", "insert"},
		"query":          {"query", "Query opcounter (diff)", "query"},
		"update":         {"update", "Update opcounter (diff)", "update"},
		"delete":         {"delete", "Delete opcounter (diff)", "delete"},
		"getmore":        {"getmore", "GetMore opcounter (diff)", "getmore"},
		"command":        {"command", "Command opcounter (diff)", "command"},
		"dirty":          {"dirty", "Cache dirty (percentage)", "% dirty"},
		"used":           {"used", "Cache used (percentage)", "% used"},
		"flushes":        {"flushes", "Number of flushes (diff)", "flushes"},
		"mapped":         {"mapped", "Mapped (size)", "mapped"},
		"vsize":          {"vsize", "Virtual (size)", "vsize"},
		"res":            {"res", "Resident (size)", "res"},
		"nonmapped":      {"nonmapped", "Non-mapped (size)", "non-mapped"},
		"faults":         {"faults", "Page faults (diff)", "faults"},
		"lrw":            {"lrw", "Lock acquire count, read|write (diff percentage)", "lr|lw %"},
		"lrwt":           {"lrwt", "Lock acquire time, read|write (diff percentage)", "lrt|lwt"},
		"locked_db":      {"locked_db", "Locked db info, '(db):(percentage)'", "locked"},
		"qrw":            {"qrw", "Queued accesses, read|write", "qr|qw"},
		"arw":            {"arw", "Active accesses, read|write", "ar|aw"},
		"net_in":         {"net_in", "Network input (size)", "netIn"},
		"net_out":        {"net_out", "Network output (size)", "netOut"},
		"conn":           {"conn", "Current connection count", "conn"},
		"set":            {"set", "FlagReplica set name", "set"},
		"repl":           {"repl", "FlagReplica set type", "repl"},
		"time":           {"time", "Time of sample", "time"},
	}
	StatHeaders = map[string]StatHeader{
		"host":           {status.ReadHost},
		"storage_engine": {status.ReadStorageEngine},
		"insert":         {status.ReadInsert},
		"query":          {status.ReadQuery},
		"update":         {status.ReadUpdate},
		"delete":         {status.ReadDelete},
		"getmore":        {status.ReadGetMore},
		"command":        {status.ReadCommand},
		"dirty":          {status.ReadDirty},
		"used":           {status.ReadUsed},
		"flushes":        {status.ReadFlushes},
		"mapped":         {status.ReadMapped},
		"vsize":          {status.ReadVSize},
		"res":            {status.ReadRes},
		"nonmapped":      {status.ReadNonMapped},
		"faults":         {status.ReadFaults},
		"lrw":            {status.ReadLRW},
		"lrwt":           {status.ReadLRWT},
		"locked_db":      {status.ReadLockedDB},
		"qrw":            {status.ReadQRW},
		"arw":            {status.ReadARW},
		"net_in":         {status.ReadNetIn},
		"net_out":        {status.ReadNetOut},
		"conn":           {status.ReadConn},
		"set":            {status.ReadSet},
		"repl":           {status.ReadRepl},
		"time":           {status.ReadTime},
	}
	CondHeaders = []struct {
		Key  string
		Flag int
	}{
		{"host", FlagHosts},
		{"insert", FlagAlways},
		{"query", FlagAlways},
		{"update", FlagAlways},
		{"delete", FlagAlways},
		{"getmore", FlagAlways},
		{"command", FlagAlways},
		{"dirty", FlagWT},
		{"used", FlagWT},
		{"flushes", FlagAlways},
		{"mapped", FlagMMAP},
		{"vsize", FlagAlways},
		{"res", FlagAlways},
		{"nonmapped", FlagMMAP | FlagAll},
		{"faults", FlagMMAP},
		{"lrw", FlagMMAP | FlagAll},
		{"lrwt", FlagMMAP | FlagAll},
		{"locked_db", FlagLocks},
		{"qrw", FlagAlways},
		{"arw", FlagAlways},
		{"net_in", FlagAlways},
		{"net_out", FlagAlways},
		{"conn", FlagAlways},
		{"set", FlagRepl},
		{"repl", FlagRepl},
		{"time", FlagAlways},
	}
)

func defaultKeyMap(index int) map[string]string {
	names := make(map[string]string)
	for k, v := range keyNames {
		names[k] = v[index]
	}
	return names
}

func DefaultKeyMap() map[string]string {
	return defaultKeyMap(0)
}

func LongKeyMap() map[string]string {
	return defaultKeyMap(1)
}

func DeprecatedKeyMap() map[string]string {
	return defaultKeyMap(2)
}
