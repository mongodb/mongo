// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"fmt"
	"io"

	mgo "github.com/10gen/llmgo"
)

// UnknownOp is not a real mongo Op but represents an unrecognized or corrupted op
type UnknownOp struct {
	Header MsgHeader
	Body   []byte
}

// Meta returns metadata about the UnknownOp, for which there is none
func (op *UnknownOp) Meta() OpMetadata {
	return OpMetadata{"", "", "", nil}
}

func (op *UnknownOp) String() string {
	return fmt.Sprintf("OpUnkown: %v", op.Header.OpCode)
}

// Abbreviated returns a serialization of the UnknownOp, abbreviated so it
// doesn't exceed the given number of characters.
func (op *UnknownOp) Abbreviated(chars int) string {
	return fmt.Sprintf("%v", op)
}

// OpCode returns the OpCode for an UnknownOp.
func (op *UnknownOp) OpCode() OpCode {
	return op.Header.OpCode
}

// FromReader extracts data from a serialized UnknownOp into its concrete structure.
func (op *UnknownOp) FromReader(r io.Reader) error {
	if op.Header.MessageLength < MsgHeaderLen {
		return nil
	}
	op.Body = make([]byte, op.Header.MessageLength-MsgHeaderLen)
	_, err := io.ReadFull(r, op.Body)
	return err
}

// Execute doesn't do anything for an UnknownOp
func (op *UnknownOp) Execute(session *mgo.Session) (*ReplyOp, error) {
	return nil, nil
}
