// Copyright (C) MongoDB, Inc. 2015-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Based on gopkg.io/mgo.v2 by Gustavo Niemeyer.
// See THIRD-PARTY-NOTICES for original license terms.

package mgo

import (
	"sync"
)

var stats *Stats
var statsMutex sync.Mutex

func SetStats(enabled bool) {
	statsMutex.Lock()
	if enabled {
		if stats == nil {
			stats = &Stats{}
		}
	} else {
		stats = nil
	}
	statsMutex.Unlock()
}

func GetStats() (snapshot Stats) {
	statsMutex.Lock()
	snapshot = *stats
	statsMutex.Unlock()
	return
}

func ResetStats() {
	statsMutex.Lock()
	debug("Resetting stats")
	old := stats
	stats = &Stats{}
	// These are absolute values:
	stats.Clusters = old.Clusters
	stats.SocketsInUse = old.SocketsInUse
	stats.SocketsAlive = old.SocketsAlive
	stats.SocketRefs = old.SocketRefs
	statsMutex.Unlock()
	return
}

type Stats struct {
	Clusters     int
	MasterConns  int
	SlaveConns   int
	SentOps      int
	ReceivedOps  int
	ReceivedDocs int
	SocketsAlive int
	SocketsInUse int
	SocketRefs   int
}

func (stats *Stats) cluster(delta int) {
	if stats != nil {
		statsMutex.Lock()
		stats.Clusters += delta
		statsMutex.Unlock()
	}
}

func (stats *Stats) conn(delta int, master bool) {
	if stats != nil {
		statsMutex.Lock()
		if master {
			stats.MasterConns += delta
		} else {
			stats.SlaveConns += delta
		}
		statsMutex.Unlock()
	}
}

func (stats *Stats) sentOps(delta int) {
	if stats != nil {
		statsMutex.Lock()
		stats.SentOps += delta
		statsMutex.Unlock()
	}
}

func (stats *Stats) receivedOps(delta int) {
	if stats != nil {
		statsMutex.Lock()
		stats.ReceivedOps += delta
		statsMutex.Unlock()
	}
}

func (stats *Stats) receivedDocs(delta int) {
	if stats != nil {
		statsMutex.Lock()
		stats.ReceivedDocs += delta
		statsMutex.Unlock()
	}
}

func (stats *Stats) socketsInUse(delta int) {
	if stats != nil {
		statsMutex.Lock()
		stats.SocketsInUse += delta
		statsMutex.Unlock()
	}
}

func (stats *Stats) socketsAlive(delta int) {
	if stats != nil {
		statsMutex.Lock()
		stats.SocketsAlive += delta
		statsMutex.Unlock()
	}
}

func (stats *Stats) socketRefs(delta int) {
	if stats != nil {
		statsMutex.Lock()
		stats.SocketRefs += delta
		statsMutex.Unlock()
	}
}
