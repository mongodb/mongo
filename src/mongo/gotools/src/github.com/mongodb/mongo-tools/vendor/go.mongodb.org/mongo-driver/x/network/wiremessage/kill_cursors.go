// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package wiremessage

import (
	"errors"
	"fmt"
	"go.mongodb.org/mongo-driver/x/bsonx"
)

// KillCursors represents the OP_KILL_CURSORS message of the MongoDB wire protocol.
type KillCursors struct {
	MsgHeader         Header
	Zero              int32
	NumberOfCursorIDs int32
	CursorIDs         []int64

	DatabaseName   string
	CollectionName string
}

// MarshalWireMessage implements the Marshaler and WireMessage interfaces.
func (kc KillCursors) MarshalWireMessage() ([]byte, error) {
	b := make([]byte, 0, kc.Len())
	return kc.AppendWireMessage(b)
}

// ValidateWireMessage implements the Validator and WireMessage interfaces.
func (kc KillCursors) ValidateWireMessage() error {
	if int(kc.MsgHeader.MessageLength) != kc.Len() {
		return errors.New("incorrect header: message length is not correct")
	}
	if kc.MsgHeader.OpCode != OpKillCursors {
		return errors.New("incorrect header: op code is not OpGetMore")
	}
	if kc.NumberOfCursorIDs != int32(len(kc.CursorIDs)) {
		return errors.New("incorrect number of cursor IDs")
	}

	return nil
}

// AppendWireMessage implements the Appender and WireMessage interfaces.
func (kc KillCursors) AppendWireMessage(b []byte) ([]byte, error) {
	var err error
	err = kc.MsgHeader.SetDefaults(kc.Len(), OpKillCursors)

	b = kc.MsgHeader.AppendHeader(b)
	b = appendInt32(b, kc.Zero)
	b = appendInt32(b, kc.NumberOfCursorIDs)
	for _, id := range kc.CursorIDs {
		b = appendInt64(b, id)
	}

	return b, err
}

// String implements the fmt.Stringer interface.
func (kc KillCursors) String() string {
	return fmt.Sprintf(
		`OP_KILL_CURSORS{MsgHeader: %s, Zero: %d, Number of Cursor IDS: %d, Cursor IDs: %v}`,
		kc.MsgHeader, kc.Zero, kc.NumberOfCursorIDs, kc.CursorIDs,
	)
}

// Len implements the WireMessage interface.
func (kc KillCursors) Len() int {
	// Header + Zero + Number IDs + 8 * Number IDs
	return 16 + 4 + 4 + int(kc.NumberOfCursorIDs*8)
}

// UnmarshalWireMessage implements the Unmarshaler interface.
func (kc *KillCursors) UnmarshalWireMessage([]byte) error {
	panic("not implemented")
}

// CommandDocument creates a BSON document representing this command.
func (kc KillCursors) CommandDocument() bsonx.Doc {
	cursors := make([]bsonx.Val, len(kc.CursorIDs))
	for i, id := range kc.CursorIDs {
		cursors[i] = bsonx.Int64(id)
	}

	return bsonx.Doc{
		{"killCursors", bsonx.String(kc.CollectionName)},
		{"cursors", bsonx.Array(cursors)},
	}
}
