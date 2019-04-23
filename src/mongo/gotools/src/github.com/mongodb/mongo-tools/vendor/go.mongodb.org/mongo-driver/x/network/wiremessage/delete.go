// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package wiremessage

import "go.mongodb.org/mongo-driver/bson"

// Delete represents the OP_DELETE message of the MongoDB wire protocol.
type Delete struct {
	MsgHeader          Header
	FullCollectionName string
	Flags              DeleteFlag
	Selector           bson.Raw
}

// MarshalWireMessage implements the Marshaler and WireMessage interfaces.
func (d Delete) MarshalWireMessage() ([]byte, error) {
	panic("not implemented")
}

// ValidateWireMessage implements the Validator and WireMessage interfaces.
func (d Delete) ValidateWireMessage() error {
	panic("not implemented")
}

// AppendWireMessage implements the Appender and WireMessage interfaces.
func (d Delete) AppendWireMessage([]byte) ([]byte, error) {
	panic("not implemented")
}

// String implements the fmt.Stringer interface.
func (d Delete) String() string {
	panic("not implemented")
}

// Len implements the WireMessage interface.
func (d Delete) Len() int {
	panic("not implemented")
}

// UnmarshalWireMessage implements the Unmarshaler interface.
func (d *Delete) UnmarshalWireMessage([]byte) error {
	panic("not implemented")
}

// DeleteFlag represents the flags on an OP_DELETE message.
type DeleteFlag int32

// These constants represent the individual flags on an OP_DELETE message.
const (
	SingleRemove DeleteFlag = 1 << iota
)
