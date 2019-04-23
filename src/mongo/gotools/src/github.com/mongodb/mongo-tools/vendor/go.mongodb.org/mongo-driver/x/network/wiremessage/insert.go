// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package wiremessage

import "go.mongodb.org/mongo-driver/bson"

// Insert represents the OP_INSERT message of the MongoDB wire protocol.
type Insert struct {
	MsgHeader          Header
	Flags              InsertFlag
	FullCollectionName string
	Documents          []bson.Raw
}

// MarshalWireMessage implements the Marshaler and WireMessage interfaces.
func (i Insert) MarshalWireMessage() ([]byte, error) {
	panic("not implemented")
}

// ValidateWireMessage implements the Validator and WireMessage interfaces.
func (i Insert) ValidateWireMessage() error {
	panic("not implemented")
}

// AppendWireMessage implements the Appender and WireMessage interfaces.
func (i Insert) AppendWireMessage([]byte) ([]byte, error) {
	panic("not implemented")
}

// String implements the fmt.Stringer interface.
func (i Insert) String() string {
	panic("not implemented")
}

// Len implements the WireMessage interface.
func (i Insert) Len() int {
	panic("not implemented")
}

// UnmarshalWireMessage implements the Unmarshaler interface.
func (i *Insert) UnmarshalWireMessage([]byte) error {
	panic("not implemented")
}

// InsertFlag represents the flags on an OP_INSERT message.
type InsertFlag int32

// These constants represent the individual flags on an OP_INSERT message.
const (
	ContinueOnError InsertFlag = 1 << iota
)
