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

// CommandReplyOp is a struct for parsing OP_COMMANDREPLY as defined here:
// https://github.com/mongodb/mongo/blob/master/src/mongo/rpc/command_reply.h.
// Although this file parses the wire protocol message into a more useable
// struct, it does not currently provide functionality to execute the operation,
// as it is not implemented fully in llmgo.
type CommandReplyOp struct {
	Header MsgHeader
	mgo.CommandReplyOp
	Docs     []bson.Raw
	Latency  time.Duration
	cursorID *int64
}

// OpCode returns the OpCode for a CommandReplyOp.
func (op *CommandReplyOp) OpCode() OpCode {
	return OpCodeCommandReply
}

// Meta returns metadata about the operation, useful for analysis of traffic.
// Currently only returns 'unknown' as it is not fully parsed and analyzed.
func (op *CommandReplyOp) Meta() OpMetadata {
	return OpMetadata{"op_commandreply",
		"",
		"",
		map[string]interface{}{
			"metadata":      op.Metadata,
			"command_reply": op.CommandReply,
			"output_docs":   op.OutputDocs,
		},
	}
}

func (op *CommandReplyOp) String() string {
	commandReplyString, metadataString, outputDocsString, err := op.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("CommandReply %v %v %v", commandReplyString, metadataString, outputDocsString)
}

// Abbreviated returns a serialization of the OpCommand, abbreviated so it
// doesn't exceed the given number of characters.
func (op *CommandReplyOp) Abbreviated(chars int) string {
	commandReplyString, metadataString, outputDocsString, err := op.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("CommandReply replay:%v metadata:%v outputdocs:%v",
		Abbreviate(commandReplyString, chars), Abbreviate(metadataString, chars),
		Abbreviate(outputDocsString, chars))
}

// getCursorID implements the Replyable interface method of the same name. It
// returns the cursorID associated with this CommandReplyOp. It returns an error
// if there is an issue unmarshalling the underlying bson. getCursorID also
// caches in the CommandReplyOp struct so that multiple calls to this function
// do not incur the cost of unmarshalling the bson.
func (op *CommandReplyOp) getCursorID() (int64, error) {
	if op.cursorID != nil {
		return *op.cursorID, nil
	}
	replyArgs := op.CommandReply.(*bson.Raw)

	id, err := getCursorID(replyArgs)
	if err != nil {
		return 0, err
	}
	op.cursorID = &id
	return *op.cursorID, nil
}

func (op *CommandReplyOp) getOpBodyString() (string, string, string, error) {
	commandReplyDoc, err := ConvertBSONValueToJSON(op.CommandReply)
	if err != nil {
		return "", "", "", fmt.Errorf("ConvertBSONValueToJSON err: %#v - %v", op, err)
	}

	commandReplyAsJSON, err := json.Marshal(commandReplyDoc)
	if err != nil {
		return "", "", "", fmt.Errorf("json marshal err: %#v - %v", op, err)
	}

	metadataDocs, err := ConvertBSONValueToJSON(op.Metadata)
	if err != nil {
		return "", "", "", fmt.Errorf("ConvertBSONValueToJSON err: %#v - %v", op, err)
	}

	metadataAsJSON, err := json.Marshal(metadataDocs)
	if err != nil {
		return "", "", "", fmt.Errorf("json marshal err: %#v - %v", op, err)
	}

	var outputDocsString string

	if len(op.OutputDocs) != 0 {
		outputDocs, err := ConvertBSONValueToJSON(op.OutputDocs)
		if err != nil {
			return "", "", "", fmt.Errorf("ConvertBSONValueToJSON err: %#v - %v", op, err)
		}

		outputDocsAsJSON, err := json.Marshal(outputDocs)
		if err != nil {
			return "", "", "", fmt.Errorf("json marshal err: %#v - %v", op, err)
		}
		outputDocsString = string(outputDocsAsJSON)
	}
	return string(commandReplyAsJSON), string(metadataAsJSON), outputDocsString, nil
}

// FromReader extracts data from a serialized CommandReplyOp into its
// concrete structure.
func (op *CommandReplyOp) FromReader(r io.Reader) error {
	commandReplyAsSlice, err := ReadDocument(r)
	if err != nil {
		return err
	}
	op.CommandReply = &bson.Raw{}
	err = bson.Unmarshal(commandReplyAsSlice, op.CommandReply)
	if err != nil {
		return err
	}

	metadataAsSlice, err := ReadDocument(r)
	if err != nil {
		return err
	}
	op.Metadata = &bson.Raw{}
	err = bson.Unmarshal(metadataAsSlice, op.Metadata)
	if err != nil {
		return err
	}

	op.OutputDocs = make([]interface{}, 0)
	for {
		docAsSlice, err := ReadDocument(r)
		if err != nil {
			if err != io.EOF {
				// Broken BSON in reply data. TODO log something here?
				return err
			}
			break
		}
		if len(docAsSlice) == 0 {
			break
		}
		doc := &bson.Raw{}
		err = bson.Unmarshal(docAsSlice, doc)
		if err != nil {
			return err
		}
		op.OutputDocs = append(op.OutputDocs, doc)
	}
	return nil
}

// Execute logs a warning and returns nil because OP_COMMANDREPLY cannot yet be
// handled fully by mongoreplay.
func (op *CommandReplyOp) Execute(socket *mgo.MongoSocket) (Replyable, error) {
	userInfoLogger.Logv(Always, "Skipping unimplemented op: OP_COMMANDREPLY")
	return nil, nil
}

func (op *CommandReplyOp) getNumReturned() int {
	return len(op.Docs)
}

func (op *CommandReplyOp) getLatencyMicros() int64 {
	return int64(op.Latency / (time.Microsecond))
}
func (op *CommandReplyOp) getErrors() []error {
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
