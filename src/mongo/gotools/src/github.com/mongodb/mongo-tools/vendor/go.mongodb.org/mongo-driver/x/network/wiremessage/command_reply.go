// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package wiremessage

import "go.mongodb.org/mongo-driver/bson"

// CommandReply represents the OP_COMMANDREPLY message of the MongoDB wire protocol.
type CommandReply struct {
	MsgHeader    Header
	Metadata     bson.Raw
	CommandReply bson.Raw
	OutputDocs   []bson.Raw
}

// MarshalWireMessage implements the Marshaler and WireMessage interfaces.
func (cr CommandReply) MarshalWireMessage() ([]byte, error) {
	panic("not implemented")
}

// ValidateWireMessage implements the Validator and WireMessage interfaces.
func (cr CommandReply) ValidateWireMessage() error {
	panic("not implemented")
}

// AppendWireMessage implements the Appender and WireMessage interfaces.
func (cr CommandReply) AppendWireMessage([]byte) ([]byte, error) {
	panic("not implemented")
}

// String implements the fmt.Stringer interface.
func (cr CommandReply) String() string {
	panic("not implemented")
}

// Len implements the WireMessage interface.
func (cr CommandReply) Len() int {
	panic("not implemented")
}

// UnmarshalWireMessage implements the Unmarshaler interface.
func (cr *CommandReply) UnmarshalWireMessage([]byte) error {
	panic("not implemented")
}
