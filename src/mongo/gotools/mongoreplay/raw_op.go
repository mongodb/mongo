// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"bytes"
	"fmt"
	"io"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

const maxBSONSize = 16 * 1024 * 1024 // 16MB - maximum BSON document size

// RawOp may be exactly the same as OpUnknown.
type RawOp struct {
	Header MsgHeader
	Body   []byte
}

func (op *RawOp) String() string {
	return fmt.Sprintf("RawOp: %v", op.Header.OpCode)
}

// Abbreviated returns a serialization of the RawOp, abbreviated so it
// doesn't exceed the given number of characters.
func (op *RawOp) Abbreviated(chars int) string {
	return fmt.Sprintf("%v", op)
}

// OpCode returns the OpCode for the op.
func (op *RawOp) OpCode() OpCode {
	return op.Header.OpCode
}

// FromReader extracts data from a serialized op into its concrete
// structure.
func (op *RawOp) FromReader(r io.Reader) error {
	if op.Header.MessageLength < MsgHeaderLen {
		return nil
	}
	if op.Header.MessageLength > MaxMessageSize {
		return fmt.Errorf("wire message size, %v, was greater then the maximum, %v bytes", op.Header.MessageLength, MaxMessageSize)
	}
	tempBody := make([]byte, op.Header.MessageLength-MsgHeaderLen)
	_, err := io.ReadFull(r, tempBody)

	if op.Body != nil {
		op.Body = append(op.Body, tempBody...)
	} else {
		op.Body = tempBody
	}
	return err
}

type CommandReplyStruct struct {
	Cursor struct {
		Id         int64    `bson:"id"`
		Ns         string   `bson:"ns"`
		FirstBatch bson.Raw `bson:"firstBatch,omitempty"`
		NextBatch  bson.Raw `bson:"nextBatch,omitempty"`
	} `bson:"cursor"`
	Ok int `bson:"ok"`
}

// ShortReplyFromReader reads an op from the given reader. It only holds on
// to header-related information and the first document.
func (op *RawOp) ShortenReply() error {
	if op.Header.MessageLength < MsgHeaderLen {
		return fmt.Errorf("expected message header to have length: %d bytes but was %d bytes", MsgHeaderLen, op.Header.MessageLength)
	}
	if op.Header.MessageLength > MaxMessageSize {
		return fmt.Errorf("wire message size, %v, was greater then the maximum, %v bytes", op.Header.MessageLength, MaxMessageSize)
	}

	switch op.Header.OpCode {
	case OpCodeReply:
		if op.Header.MessageLength <= 20+MsgHeaderLen {
			//there are no reply docs
			return nil
		}
		firstDocSize := getInt32(op.Body, 20+MsgHeaderLen)
		if 20+MsgHeaderLen+int(firstDocSize) > len(op.Body) || firstDocSize > maxBSONSize {
			return fmt.Errorf("the size of the first document is greater then the size of the message")
		}
		op.Body = op.Body[0:(20 + MsgHeaderLen + firstDocSize)]

	case OpCodeCommandReply:
		// unmarshal the needed fields for replacing into the buffer
		commandReply := &CommandReplyStruct{}

		err := bson.Unmarshal(op.Body[MsgHeaderLen:], commandReply)
		if err != nil {
			return fmt.Errorf("unmarshaling op to shorten: %v", err)
		}
		switch {
		case commandReply.Cursor.FirstBatch.Data != nil:
			commandReply.Cursor.FirstBatch.Data, _ = bson.Marshal([0]byte{})

		case commandReply.Cursor.NextBatch.Data != nil:
			commandReply.Cursor.NextBatch.Data, _ = bson.Marshal([0]byte{})

		default:
			// it's not a findReply so we don't care about it
			return nil
		}

		out, err := bson.Marshal(commandReply)
		if err != nil {
			return err
		}

		// calculate the new sizes for offsets into the new buffer
		commandReplySize := getInt32(op.Body, MsgHeaderLen)
		newCommandReplySize := getInt32(out, 0)
		sizeDiff := commandReplySize - newCommandReplySize
		newSize := op.Header.MessageLength - sizeDiff
		newBody := make([]byte, newSize)

		// copy the new data into a buffer that will replace the old buffer
		copy(newBody, op.Body[:MsgHeaderLen])
		copy(newBody[MsgHeaderLen:], out)
		copy(newBody[MsgHeaderLen+newCommandReplySize:], op.Body[MsgHeaderLen+commandReplySize:])
		// update the size of this message in the headers
		SetInt32(newBody, 0, newSize)
		op.Header.MessageLength = newSize
		op.Body = newBody

	default:
		return fmt.Errorf("unexpected op type : %v", op.Header.OpCode)
	}
	return nil
}

// Parse returns the underlying op from its given RawOp form.
func (op *RawOp) Parse() (Op, error) {
	if op.Header.OpCode == OpCodeCompressed {
		newMsg, err := mgo.DecompressMessage(op.Body)
		if err != nil {
			return nil, err
		}
		op.Header.FromWire(newMsg)
		op.Body = newMsg
	}

	var parsedOp Op
	switch op.Header.OpCode {
	case OpCodeQuery:
		parsedOp = &QueryOp{Header: op.Header}
	case OpCodeReply:
		parsedOp = &ReplyOp{Header: op.Header}
	case OpCodeGetMore:
		parsedOp = &GetMoreOp{Header: op.Header}
	case OpCodeInsert:
		parsedOp = &InsertOp{Header: op.Header}
	case OpCodeKillCursors:
		parsedOp = &KillCursorsOp{Header: op.Header}
	case OpCodeDelete:
		parsedOp = &DeleteOp{Header: op.Header}
	case OpCodeUpdate:
		parsedOp = &UpdateOp{Header: op.Header}
	case OpCodeCommand:
		parsedOp = &CommandOp{Header: op.Header}
	case OpCodeCommandReply:
		parsedOp = &CommandReplyOp{Header: op.Header}
	case OpCodeMessage:
		parsedOp = &MsgOp{Header: op.Header}
	default:
		return nil, nil
	}
	reader := bytes.NewReader(op.Body[MsgHeaderLen:])
	err := parsedOp.FromReader(reader)
	if err != nil {
		return nil, err
	}

	parsedOp, err = maybeChangeOpToGetMore(parsedOp)
	if err != nil {
		return nil, err
	}

	if op, ok := parsedOp.(*MsgOp); ok && op.Header.ResponseTo != 0 {
		op.CommandName = "reply"
		parsedOp = &MsgOpReply{
			MsgOp: *op,
		}
	}
	return parsedOp, nil

}

// maybeChangeOpToGetMore determines if the op is a more specific case of the Op
// interface and should be returned as a type of getmore
func maybeChangeOpToGetMore(parsedOp Op) (Op, error) {

	switch castOp := parsedOp.(type) {
	case *CommandOp:
		if castOp.CommandName == "getMore" {
			return &CommandGetMore{
				CommandOp: *castOp,
			}, nil
		}
	case *MsgOp:
		id, err := castOp.getCommandName()
		if err != nil {
			return nil, err
		}
		if id == "getMore" {
			return &MsgOpGetMore{
				MsgOp: *castOp,
			}, nil
		}
	}

	// not any special case, return the original op
	return parsedOp, nil
}
