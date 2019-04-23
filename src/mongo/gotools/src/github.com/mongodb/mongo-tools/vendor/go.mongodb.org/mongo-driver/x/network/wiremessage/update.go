// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package wiremessage

import "go.mongodb.org/mongo-driver/bson"

// Update represents the OP_UPDATE message of the MongoDB wire protocol.
type Update struct {
	MsgHeader          Header
	FullCollectionName string
	Flags              UpdateFlag
	Selector           bson.Raw
	Update             bson.Raw
}

// MarshalWireMessage implements the Marshaler and WireMessage interfaces.
func (u Update) MarshalWireMessage() ([]byte, error) {
	panic("not implemented")
}

// ValidateWireMessage implements the Validator and WireMessage interfaces.
func (u Update) ValidateWireMessage() error {
	panic("not implemented")
}

// AppendWireMessage implements the Appender and WireMessage interfaces.
func (u Update) AppendWireMessage([]byte) ([]byte, error) {
	panic("not implemented")
}

// String implements the fmt.Stringer interface.
func (u Update) String() string {
	panic("not implemented")
}

// Len implements the WireMessage interface.
func (u Update) Len() int {
	panic("not implemented")
}

// UnmarshalWireMessage implements the Unmarshaler interface.
func (u *Update) UnmarshalWireMessage([]byte) error {
	panic("not implemented")
}

// UpdateFlag represents the flags on an OP_UPDATE message.
type UpdateFlag int32

// These constants represent the individual flags on an OP_UPDATE message.
const (
	Upsert UpdateFlag = 1 << iota
	MultiUpdate
)
