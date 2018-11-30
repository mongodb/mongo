// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

// RecordedOp stores an op in addition to record/playback -related metadata
type RecordedOp struct {
	RawOp
	Seen                *PreciseTime
	PlayAt              *PreciseTime `bson:",omitempty"`
	EOF                 bool         `bson:",omitempty"`
	SrcEndpoint         string
	DstEndpoint         string
	SeenConnectionNum   int64
	PlayedConnectionNum int64
	PlayedAt            *PreciseTime `bson:",omitempty"`
	Generation          int
	Order               int64
}

// ConnectionString gives a serialized representation of the endpoints
func (op *RecordedOp) ConnectionString() string {
	return op.SrcEndpoint + "->" + op.DstEndpoint
}

// ReversedConnectionString gives a serialized representation of the endpoints,
// in reversed order
func (op *RecordedOp) ReversedConnectionString() string {
	return op.DstEndpoint + "->" + op.SrcEndpoint
}

type orderedOps []RecordedOp

func (o orderedOps) Len() int {
	return len(o)
}

func (o orderedOps) Less(i, j int) bool {
	return o[i].Seen.Before(o[j].Seen.Time)
}

func (o orderedOps) Swap(i, j int) {
	o[i], o[j] = o[j], o[i]
}

func (o *orderedOps) Pop() interface{} {
	i := len(*o) - 1
	op := (*o)[i]
	*o = (*o)[:i]
	return op
}

func (o *orderedOps) Push(op interface{}) {
	*o = append(*o, op.(RecordedOp))
}

type opKey struct {
	driverEndpoint, serverEndpoint string
	opID                           int32
}
