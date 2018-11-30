// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package status

import "time"

type ServerStatus struct {
	SampleTime         time.Time              `bson:""`
	Flattened          map[string]interface{} `bson:""`
	Host               string                 `bson:"host"`
	Version            string                 `bson:"version"`
	Process            string                 `bson:"process"`
	Pid                int64                  `bson:"pid"`
	Uptime             int64                  `bson:"uptime"`
	UptimeMillis       int64                  `bson:"uptimeMillis"`
	UptimeEstimate     int64                  `bson:"uptimeEstimate"`
	LocalTime          time.Time              `bson:"localTime"`
	Asserts            map[string]int64       `bson:"asserts"`
	BackgroundFlushing *FlushStats            `bson:"backgroundFlushing"`
	ExtraInfo          *ExtraInfo             `bson:"extra_info"`
	Connections        *ConnectionStats       `bson:"connections"`
	Dur                *DurStats              `bson:"dur"`
	GlobalLock         *GlobalLockStats       `bson:"globalLock"`
	Locks              map[string]LockStats   `bson:"locks,omitempty"`
	Network            *NetworkStats          `bson:"network"`
	Opcounters         *OpcountStats          `bson:"opcounters"`
	OpcountersRepl     *OpcountStats          `bson:"opcountersRepl"`
	RecordStats        *DBRecordStats         `bson:"recordStats"`
	Mem                *MemStats              `bson:"mem"`
	Repl               *ReplStatus            `bson:"repl"`
	ShardCursorType    map[string]interface{} `bson:"shardCursorType"`
	StorageEngine      map[string]string      `bson:"storageEngine"`
	WiredTiger         *WiredTiger            `bson:"wiredTiger"`
}

// WiredTiger stores information related to the WiredTiger storage engine.
type WiredTiger struct {
	Transaction TransactionStats       `bson:"transaction"`
	Concurrent  ConcurrentTransactions `bson:"concurrentTransactions"`
	Cache       CacheStats             `bson:"cache"`
}

type ConcurrentTransactions struct {
	Write ConcurrentTransStats `bson:"write"`
	Read  ConcurrentTransStats `bson:"read"`
}

type ConcurrentTransStats struct {
	Out int64 `bson:"out"`
}

// CacheStats stores cache statistics for WiredTiger.
type CacheStats struct {
	TrackedDirtyBytes  int64 `bson:"tracked dirty bytes in the cache"`
	CurrentCachedBytes int64 `bson:"bytes currently in the cache"`
	MaxBytesConfigured int64 `bson:"maximum bytes configured"`
}

// TransactionStats stores transaction checkpoints in WiredTiger.
type TransactionStats struct {
	TransCheckpoints int64 `bson:"transaction checkpoints"`
}

// ReplStatus stores data related to replica sets.
type ReplStatus struct {
	SetName      string      `bson:"setName"`
	IsMaster     interface{} `bson:"ismaster"`
	Secondary    interface{} `bson:"secondary"`
	IsReplicaSet interface{} `bson:"isreplicaset"`
	ArbiterOnly  interface{} `bson:"arbiterOnly"`
	Hosts        []string    `bson:"hosts"`
	Passives     []string    `bson:"passives"`
	Me           string      `bson:"me"`
}

// DBRecordStats stores data related to memory operations across databases.
type DBRecordStats struct {
	AccessesNotInMemory       int64                     `bson:"accessesNotInMemory"`
	PageFaultExceptionsThrown int64                     `bson:"pageFaultExceptionsThrown"`
	DBRecordAccesses          map[string]RecordAccesses `bson:",inline"`
}

// RecordAccesses stores data related to memory operations scoped to a database.
type RecordAccesses struct {
	AccessesNotInMemory       int64 `bson:"accessesNotInMemory"`
	PageFaultExceptionsThrown int64 `bson:"pageFaultExceptionsThrown"`
}

// MemStats stores data related to memory statistics.
type MemStats struct {
	Bits              int64       `bson:"bits"`
	Resident          int64       `bson:"resident"`
	Virtual           int64       `bson:"virtual"`
	Supported         interface{} `bson:"supported"`
	Mapped            int64       `bson:"mapped"`
	MappedWithJournal int64       `bson:"mappedWithJournal"`
}

// FlushStats stores information about memory flushes.
type FlushStats struct {
	Flushes      int64     `bson:"flushes"`
	TotalMs      int64     `bson:"total_ms"`
	AverageMs    float64   `bson:"average_ms"`
	LastMs       int64     `bson:"last_ms"`
	LastFinished time.Time `bson:"last_finished"`
}

// ConnectionStats stores information related to incoming database connections.
type ConnectionStats struct {
	Current      int64 `bson:"current"`
	Available    int64 `bson:"available"`
	TotalCreated int64 `bson:"totalCreated"`
}

// DurTiming stores information related to journaling.
type DurTiming struct {
	Dt               int64 `bson:"dt"`
	PrepLogBuffer    int64 `bson:"prepLogBuffer"`
	WriteToJournal   int64 `bson:"writeToJournal"`
	WriteToDataFiles int64 `bson:"writeToDataFiles"`
	RemapPrivateView int64 `bson:"remapPrivateView"`
}

// DurStats stores information related to journaling statistics.
type DurStats struct {
	Commits            int64 `bson:"commits"`
	JournaledMB        int64 `bson:"journaledMB"`
	WriteToDataFilesMB int64 `bson:"writeToDataFilesMB"`
	Compression        int64 `bson:"compression"`
	CommitsInWriteLock int64 `bson:"commitsInWriteLock"`
	EarlyCommits       int64 `bson:"earlyCommits"`
	TimeMs             DurTiming
}

// QueueStats stores the number of queued read/write operations.
type QueueStats struct {
	Total   int64 `bson:"total"`
	Readers int64 `bson:"readers"`
	Writers int64 `bson:"writers"`
}

// ClientStats stores the number of active read/write operations.
type ClientStats struct {
	Total   int64 `bson:"total"`
	Readers int64 `bson:"readers"`
	Writers int64 `bson:"writers"`
}

// GlobalLockStats stores information related locks in the MMAP storage engine.
type GlobalLockStats struct {
	TotalTime     int64        `bson:"totalTime"`
	LockTime      int64        `bson:"lockTime"`
	CurrentQueue  *QueueStats  `bson:"currentQueue"`
	ActiveClients *ClientStats `bson:"activeClients"`
}

// NetworkStats stores information related to network traffic.
type NetworkStats struct {
	BytesIn     int64 `bson:"bytesIn"`
	BytesOut    int64 `bson:"bytesOut"`
	NumRequests int64 `bson:"numRequests"`
}

// OpcountStats stores information related to comamnds and basic CRUD operations.
type OpcountStats struct {
	Insert  int64 `bson:"insert"`
	Query   int64 `bson:"query"`
	Update  int64 `bson:"update"`
	Delete  int64 `bson:"delete"`
	GetMore int64 `bson:"getmore"`
	Command int64 `bson:"command"`
}

// ReadWriteLockTimes stores time spent holding read/write locks.
type ReadWriteLockTimes struct {
	Read       int64 `bson:"R"`
	Write      int64 `bson:"W"`
	ReadLower  int64 `bson:"r"`
	WriteLower int64 `bson:"w"`
}

// LockStats stores information related to time spent acquiring/holding locks
// for a given database.
type LockStats struct {
	TimeLockedMicros    ReadWriteLockTimes `bson:"timeLockedMicros"`
	TimeAcquiringMicros ReadWriteLockTimes `bson:"timeAcquiringMicros"`

	// AcquireCount and AcquireWaitCount are new fields of the lock stats only populated on 3.0 or newer.
	// Typed as a pointer so that if it is nil, mongostat can assume the field is not populated
	// with real namespace data.
	AcquireCount     *ReadWriteLockTimes `bson:"acquireCount,omitempty"`
	AcquireWaitCount *ReadWriteLockTimes `bson:"acquireWaitCount,omitempty"`
}

// ExtraInfo stores additional platform specific information.
type ExtraInfo struct {
	PageFaults *int64 `bson:"page_faults"`
}

// NodeError pairs an error with a hostname
type NodeError struct {
	Host string
	err  error
}

func (ne *NodeError) Error() string {
	return ne.err.Error()
}

func NewNodeError(host string, err error) *NodeError {
	return &NodeError{
		err:  err,
		Host: host,
	}
}

// Flatten takes a map and returns a new one where nested maps are replaced
// by dot-delimited keys.
func Flatten(m map[string]interface{}) map[string]interface{} {
	o := make(map[string]interface{})
	for k, v := range m {
		switch child := v.(type) {
		case map[string]interface{}:
			nm := Flatten(child)
			for nk, nv := range nm {
				o[k+"."+nk] = nv
			}
		default:
			o[k] = v
		}
	}
	return o
}
