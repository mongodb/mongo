// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"fmt"
	"io"
	"time"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

// GetMoreOp is used to query the database for documents in a collection.
// http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#op-get-more
type GetMoreOp struct {
	Header MsgHeader
	mgo.GetMoreOp
}

// OpCode returns the OpCode for a GetMoreOp.
func (op *GetMoreOp) OpCode() OpCode {
	return OpCodeGetMore
}

// Meta returns metadata about the GetMoreOp, useful for analysis of traffic.
func (op *GetMoreOp) Meta() OpMetadata {
	return OpMetadata{"getmore", op.Collection, "", op.CursorId}
}

func (op *GetMoreOp) String() string {
	return fmt.Sprintf("GetMore ns:%v limit:%v cursorID:%v", op.Collection, op.Limit, op.CursorId)
}

// Abbreviated returns a serialization of the GetMoreOp, abbreviated so it
// doesn't exceed the given number of characters.
func (op *GetMoreOp) Abbreviated(chars int) string {
	return fmt.Sprintf("%v", op)
}

// getCursorIDs implements the cursorsRewriteable interface method. It returns
// an array with one element, containing the cursorID from this getmore.
func (op *GetMoreOp) getCursorIDs() ([]int64, error) {
	return []int64{op.CursorId}, nil
}

// setCursorIDs implements the cursorsRewriteable interface method. It takes an
// array of cursorIDs to replace the current cursor with. If this array is
// longer than 1, it returns an error because getmores can only have - and
// therefore rewrite - one cursor.
func (op *GetMoreOp) setCursorIDs(newCursorIDs []int64) error {
	var newCursorID int64

	if len(newCursorIDs) > 1 {
		return fmt.Errorf("rewriting getmore command cursorIDs requires 1 id, received: %d", len(newCursorIDs))
	}
	if len(newCursorIDs) < 1 {
		newCursorID = 0
	} else {
		newCursorID = newCursorIDs[0]
	}
	op.GetMoreOp.CursorId = newCursorID
	return nil
}

// FromReader extracts data from a serialized GetMoreOp into its concrete
// structure.
func (op *GetMoreOp) FromReader(r io.Reader) error {
	var b [12]byte
	if _, err := io.ReadFull(r, b[:4]); err != nil {
		return err
	}
	name, err := readCStringFromReader(r)
	if err != nil {
		return err
	}
	op.Collection = string(name)
	if _, err := io.ReadFull(r, b[:12]); err != nil {
		return err
	}
	op.Limit = getInt32(b[:], 0)
	op.CursorId = getInt64(b[:], 4)
	return nil
}

// Execute performs the GetMoreOp on a given session, yielding the reply when
// successful (and an error otherwise).
func (op *GetMoreOp) Execute(socket *mgo.MongoSocket) (Replyable, error) {
	before := time.Now()

	_, _, data, resultReply, err := mgo.ExecOpWithReply(socket, &op.GetMoreOp)
	after := time.Now()

	mgoReply, ok := resultReply.(*mgo.ReplyOp)
	if !ok {
		panic("reply from execution was not the correct type")
	}

	reply := &ReplyOp{
		ReplyOp: *mgoReply,
		Docs:    make([]bson.Raw, 0, len(data)),
	}

	for _, d := range data {
		dataDoc := bson.Raw{}
		err = bson.Unmarshal(d, &dataDoc)
		if err != nil {
			return nil, err
		}
		reply.Docs = append(reply.Docs, dataDoc)
	}

	reply.Latency = after.Sub(before)
	return reply, nil
}
