// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package wiremessage

import (
	"errors"
	"fmt"
	"strings"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/bsonx"
)

// Reply represents the OP_REPLY message of the MongoDB wire protocol.
type Reply struct {
	MsgHeader      Header
	ResponseFlags  ReplyFlag
	CursorID       int64
	StartingFrom   int32
	NumberReturned int32
	Documents      []bson.Raw
}

// MarshalWireMessage implements the Marshaler and WireMessage interfaces.
//
// See AppendWireMessage for a description of the rules this method follows.
func (r Reply) MarshalWireMessage() ([]byte, error) {
	b := make([]byte, 0, r.Len())
	return r.AppendWireMessage(b)
}

// ValidateWireMessage implements the Validator and WireMessage interfaces.
func (r Reply) ValidateWireMessage() error {
	if int(r.MsgHeader.MessageLength) != r.Len() {
		return errors.New("incorrect header: message length is not correct")
	}
	if r.MsgHeader.OpCode != OpReply {
		return errors.New("incorrect header: op code is not OpReply")
	}

	return nil
}

// AppendWireMessage implements the Appender and WireMessage interfaces.
//
// AppendWireMessage will set the MessageLength property of the MsgHeader
// if it is zero. It will also set the OpCode to OpQuery if the OpCode is
// zero. If either of these properties are non-zero and not correct, this
// method will return both the []byte with the wire message appended to it
// and an invalid header error.
func (r Reply) AppendWireMessage(b []byte) ([]byte, error) {
	var err error
	err = r.MsgHeader.SetDefaults(r.Len(), OpReply)

	b = r.MsgHeader.AppendHeader(b)
	b = appendInt32(b, int32(r.ResponseFlags))
	b = appendInt64(b, r.CursorID)
	b = appendInt32(b, r.StartingFrom)
	b = appendInt32(b, r.NumberReturned)
	for _, d := range r.Documents {
		b = append(b, d...)
	}
	return b, err
}

// String implements the fmt.Stringer interface.
func (r Reply) String() string {
	return fmt.Sprintf(
		`OP_REPLY{MsgHeader: %s, ResponseFlags: %s, CursorID: %d, StartingFrom: %d, NumberReturned: %d, Documents: %v}`,
		r.MsgHeader, r.ResponseFlags, r.CursorID, r.StartingFrom, r.NumberReturned, r.Documents,
	)
}

// Len implements the WireMessage interface.
func (r Reply) Len() int {
	// Header + Flags + CursorID + StartingFrom + NumberReturned + Length of Length of Documents
	docsLen := 0
	for _, d := range r.Documents {
		docsLen += len(d)
	}
	return 16 + 4 + 8 + 4 + 4 + docsLen
}

// UnmarshalWireMessage implements the Unmarshaler interface.
func (r *Reply) UnmarshalWireMessage(b []byte) error {
	var err error
	r.MsgHeader, err = ReadHeader(b, 0)
	if err != nil {
		return err
	}
	if r.MsgHeader.MessageLength < 36 {
		return errors.New("invalid OP_REPLY: header length too small")
	}
	if len(b) < int(r.MsgHeader.MessageLength) {
		return errors.New("invalid OP_REPLY: []byte too small")
	}

	r.ResponseFlags = ReplyFlag(readInt32(b, 16))
	r.CursorID = readInt64(b, 20)
	r.StartingFrom = readInt32(b, 28)
	r.NumberReturned = readInt32(b, 32)
	pos := 36
	for pos < len(b) {
		rdr, size, err := readDocument(b, int32(pos))
		if err.Message != "" {
			err.Type = ErrOpReply
			return err
		}
		r.Documents = append(r.Documents, rdr)
		pos += size
	}

	return nil
}

// GetMainLegacyDocument constructs and returns a BSON document for this reply.
func (r *Reply) GetMainLegacyDocument(fullCollectionName string) (bsonx.Doc, error) {
	if r.ResponseFlags&CursorNotFound > 0 {
		fmt.Println("cursor not found err")
		return bsonx.Doc{
			{"ok", bsonx.Int32(0)},
		}, nil
	}
	if r.ResponseFlags&QueryFailure > 0 {
		firstDoc := r.Documents[0]
		return bsonx.Doc{
			{"ok", bsonx.Int32(0)},
			{"errmsg", bsonx.String(firstDoc.Lookup("$err").StringValue())},
			{"code", bsonx.Int32(firstDoc.Lookup("code").Int32())},
		}, nil
	}

	doc := bsonx.Doc{
		{"ok", bsonx.Int32(1)},
	}

	batchStr := "firstBatch"
	if r.StartingFrom != 0 {
		batchStr = "nextBatch"
	}

	batchArr := make([]bsonx.Val, len(r.Documents))
	for i, docRaw := range r.Documents {
		doc, err := bsonx.ReadDoc(docRaw)
		if err != nil {
			return nil, err
		}

		batchArr[i] = bsonx.Document(doc)
	}

	cursorDoc := bsonx.Doc{
		{"id", bsonx.Int64(r.CursorID)},
		{"ns", bsonx.String(fullCollectionName)},
		{batchStr, bsonx.Array(batchArr)},
	}

	doc = doc.Append("cursor", bsonx.Document(cursorDoc))
	return doc, nil
}

// GetMainDocument returns the main BSON document for this reply.
func (r *Reply) GetMainDocument() (bsonx.Doc, error) {
	return bsonx.ReadDoc([]byte(r.Documents[0]))
}

// ReplyFlag represents the flags of an OP_REPLY message.
type ReplyFlag int32

// These constants represent the individual flags of an OP_REPLY message.
const (
	CursorNotFound ReplyFlag = 1 << iota
	QueryFailure
	ShardConfigStale
	AwaitCapable
)

// String implements the fmt.Stringer interface.
func (rf ReplyFlag) String() string {
	strs := make([]string, 0)
	if rf&CursorNotFound == CursorNotFound {
		strs = append(strs, "CursorNotFound")
	}
	if rf&QueryFailure == QueryFailure {
		strs = append(strs, "QueryFailure")
	}
	if rf&ShardConfigStale == ShardConfigStale {
		strs = append(strs, "ShardConfigStale")
	}
	if rf&AwaitCapable == AwaitCapable {
		strs = append(strs, "AwaitCapable")
	}
	str := "["
	str += strings.Join(strs, ", ")
	str += "]"
	return str
}
