// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"encoding/json"
	"fmt"
	"io"
	"time"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

// ReplyOp is sent by the database in response to an QueryOp or OpGetMore message.
// http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#op-reply
type ReplyOp struct {
	Header MsgHeader
	mgo.ReplyOp
	Docs         []bson.Raw
	Latency      time.Duration
	cursorCached bool
}

// Meta returns metadata about the ReplyOp, useful for analysis of traffic.
func (op *ReplyOp) Meta() OpMetadata {
	var resultDocs interface{} = op.Docs

	// If the reply only contains a single doc, just use that doc instead of
	// also including the array containing it.
	if len(op.Docs) == 1 {
		resultDocs = op.Docs[0]
	}
	return OpMetadata{"reply", "", "", resultDocs}
}

func (op *ReplyOp) String() string {
	if op == nil {
		return "Reply NIL"
	}
	return fmt.Sprintf("ReplyOp reply:[flags:%v, cursorid:%v, first:%v ndocs:%v] docs:%v",
		op.Flags, op.CursorId, op.FirstDoc, op.ReplyDocs,
		stringifyReplyDocs(op.Docs),
	)
}

// Abbreviated returns a serialization of the ReplyOp, abbreviated so it doesn't
// exceed the given number of characters.
func (op *ReplyOp) Abbreviated(chars int) string {
	if op == nil {
		return "Reply NIL"
	}
	return fmt.Sprintf("ReplyOp reply:[flags:%v, cursorid:%v, first:%v ndocs:%v] docs:%v",
		op.Flags, op.CursorId, op.FirstDoc, op.ReplyDocs,
		Abbreviate(stringifyReplyDocs(op.Docs), chars),
	)
}

// OpCode returns the OpCode for a ReplyOp.
func (op *ReplyOp) OpCode() OpCode {
	return OpCodeReply
}

// FromReader extracts data from a serialized ReplyOp into its concrete structure.
func (op *ReplyOp) FromReader(r io.Reader) error {
	var b [20]byte
	if _, err := io.ReadFull(r, b[:]); err != nil {
		return err
	}
	op.Flags = uint32(getInt32(b[:], 0))
	op.CursorId = getInt64(b[:], 4)
	op.FirstDoc = getInt32(b[:], 12)
	op.ReplyDocs = getInt32(b[:], 16)
	op.Docs = []bson.Raw{}

	// read as many docs as we can from the reader
	for {
		docBytes, err := ReadDocument(r)
		if err != nil {
			if err != io.EOF {
				// Broken BSON in reply data. TODO log something here?
				return err
			}
			break
		}
		if len(docBytes) == 0 {
			break
		}
		nextDoc := bson.Raw{}
		err = bson.Unmarshal(docBytes, &nextDoc)
		if err != nil {
			// Unmarshaling []byte to bson.Raw should never ever fail.
			panic("failed to unmarshal []byte to Raw")
		}
		op.Docs = append(op.Docs, nextDoc)
	}

	return nil
}

// Execute performs the ReplyOp on a given socket, yielding the reply when
// successful (and an error otherwise).
func (op *ReplyOp) Execute(socket *mgo.MongoSocket) (Replyable, error) {
	return nil, nil
}

func stringifyReplyDocs(d []bson.Raw) string {
	if len(d) == 0 {
		return "[empty]"
	}
	docsConverted, err := ConvertBSONValueToJSON(d)
	if err != nil {
		return fmt.Sprintf("ConvertBSONValueToJSON err on reply docs: %v", err)
	}
	asJSON, err := json.Marshal(docsConverted)
	if err != nil {
		return fmt.Sprintf("json marshal err on reply docs: %v", err)
	}
	return string(asJSON)
}

// getCursorID implements the Replyable interface method. It returns the
// cursorID stored in this reply. It returns an error if there is an issue
// unmarshaling the underlying bson. It caches the cursorID in the ReplyOp
// struct so that subsequent calls to this function do not incur cost of
// unmarshalling the bson each time.
func (op *ReplyOp) getCursorID() (int64, error) {
	if op.CursorId != 0 || op.cursorCached {
		return op.CursorId, nil
	}
	op.cursorCached = true
	if len(op.Docs) != 1 {
		return 0, nil
	}
	doc := &struct {
		Cursor struct {
			ID int64 `bson:"id"`
		} `bson:"cursor"`
	}{}
	err := op.Docs[0].Unmarshal(&doc)
	if err != nil {
		// can happen if there's corrupt bson in the doc.
		return 0, fmt.Errorf("failed to unmarshal raw into bson.M: %v", err)
	}
	op.CursorId = doc.Cursor.ID

	return op.CursorId, nil
}

func (op *ReplyOp) getLatencyMicros() int64 {
	return int64(op.Latency / (time.Microsecond))
}

func (op *ReplyOp) getNumReturned() int {
	return len(op.Docs)
}

func (op *ReplyOp) getErrors() []error {
	if len(op.Docs) == 0 {
		return nil
	}

	firstDoc := bson.D{}
	err := op.Docs[0].Unmarshal(&firstDoc)
	if err != nil {
		panic("failed to unmarshal Raw into bson.D")
	}
	return extractErrorsFromDoc(&firstDoc)
}
