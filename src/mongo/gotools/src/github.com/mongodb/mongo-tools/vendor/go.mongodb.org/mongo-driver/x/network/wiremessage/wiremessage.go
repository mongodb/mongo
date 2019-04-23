// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package wiremessage contains types for speaking the MongoDB Wire Protocol. Since this low
// level library is meant to be used in the context of a driver and in the context of a server
// all of the flags and types of the wire protocol are implemented. For each op there are two
// corresponding implementations. One prefixed with Immutable which can be created by casting a
// []byte to the type, and another prefixed with Mutable that is a struct with methods to mutate
// the op.
package wiremessage // import "go.mongodb.org/mongo-driver/x/network/wiremessage"

import (
	"context"
	"errors"
	"fmt"
	"io"
	"sync/atomic"
)

// ErrInvalidMessageLength is returned when the provided message length is too small to be valid.
var ErrInvalidMessageLength = errors.New("the message length is too small, it must be at least 16")

// ErrUnknownOpCode is returned when the provided opcode is not a valid opcode.
var ErrUnknownOpCode = errors.New("the opcode is unknown")

var globalRequestID int32

// CurrentRequestID returns the current request ID.
func CurrentRequestID() int32 { return atomic.LoadInt32(&globalRequestID) }

// NextRequestID returns the next request ID.
func NextRequestID() int32 { return atomic.AddInt32(&globalRequestID, 1) }

// Error represents an error related to wire protocol messages.
type Error struct {
	Type    ErrorType
	Message string
}

// Error implements the err interface.
func (e Error) Error() string {
	return e.Message
}

// ErrorType is the type of error, which indicates from which part of the code
// the error originated.
type ErrorType uint16

// These constants are the types of errors exposed by this package.
const (
	ErrNil ErrorType = iota
	ErrHeader
	ErrOpQuery
	ErrOpReply
	ErrOpCompressed
	ErrOpMsg
	ErrRead
)

// OpCode represents a MongoDB wire protocol opcode.
type OpCode int32

// These constants are the valid opcodes for the version of the wireprotocol
// supported by this library. The skipped OpCodes are historical OpCodes that
// are no longer used.
const (
	OpReply        OpCode = 1
	_              OpCode = 1001
	OpUpdate       OpCode = 2001
	OpInsert       OpCode = 2002
	_              OpCode = 2003
	OpQuery        OpCode = 2004
	OpGetMore      OpCode = 2005
	OpDelete       OpCode = 2006
	OpKillCursors  OpCode = 2007
	OpCommand      OpCode = 2010
	OpCommandReply OpCode = 2011
	OpCompressed   OpCode = 2012
	OpMsg          OpCode = 2013
)

// String implements the fmt.Stringer interface.
func (oc OpCode) String() string {
	switch oc {
	case OpReply:
		return "OP_REPLY"
	case OpUpdate:
		return "OP_UPDATE"
	case OpInsert:
		return "OP_INSERT"
	case OpQuery:
		return "OP_QUERY"
	case OpGetMore:
		return "OP_GET_MORE"
	case OpDelete:
		return "OP_DELETE"
	case OpKillCursors:
		return "OP_KILL_CURSORS"
	case OpCommand:
		return "OP_COMMAND"
	case OpCommandReply:
		return "OP_COMMANDREPLY"
	case OpCompressed:
		return "OP_COMPRESSED"
	case OpMsg:
		return "OP_MSG"
	default:
		return "<invalid opcode>"
	}
}

// WireMessage represents a message in the MongoDB wire protocol.
type WireMessage interface {
	Marshaler
	Validator
	Appender
	fmt.Stringer

	// Len returns the length in bytes of this WireMessage.
	Len() int
}

// Validator is the interface implemented by types that can validate
// themselves as a MongoDB wire protocol message.
type Validator interface {
	ValidateWireMessage() error
}

// Marshaler is the interface implemented by types that can marshal
// themselves into a valid MongoDB wire protocol message.
type Marshaler interface {
	MarshalWireMessage() ([]byte, error)
}

// Appender is the interface implemented by types that can append themselves, as
// a MongoDB wire protocol message, to the provided slice of bytes.
type Appender interface {
	AppendWireMessage([]byte) ([]byte, error)
}

// Unmarshaler is the interface implemented by types that can unmarshal a
// MongoDB wire protocol message version of themselves. The input can be
// assumed to be a valid MongoDB wire protocol message. UnmarshalWireMessage
// must copy the data if it wishes to retain the data after returning.
type Unmarshaler interface {
	UnmarshalWireMessage([]byte) error
}

// Writer is the interface implemented by types that can have WireMessages
// written to them.
//
// Implementation must obey the cancellation, timeouts, and deadlines of the
// provided context.Context object.
type Writer interface {
	WriteWireMessage(context.Context, WireMessage) error
}

// Reader is the interface implemented by types that can have WireMessages
// read from them.
//
// Implementation must obey the cancellation, timeouts, and deadlines of the
// provided context.Context object.
type Reader interface {
	ReadWireMessage(context.Context) (WireMessage, error)
}

// ReadWriter is the interface implemented by types that can both read and write
// WireMessages.
type ReadWriter interface {
	Reader
	Writer
}

// ReadWriteCloser is the interface implemented by types that can read and write
// WireMessages and can also be closed.
type ReadWriteCloser interface {
	Reader
	Writer
	io.Closer
}

// Transformer is the interface implemented by types that can alter a WireMessage.
// Implementations should not directly alter the provided WireMessage and instead
// make a copy of the message, alter it, and returned the new message.
type Transformer interface {
	TransformWireMessage(WireMessage) (WireMessage, error)
}

// ReadFrom will read a single WireMessage from the given io.Reader. This function will
// validate the WireMessage. If the WireMessage is not valid, this method will
// return both the error and the invalid WireMessage. If another type of processing
// error occurs, WireMessage will be nil.
//
// This function will return the immutable versions of wire protocol messages. The
// Convert function can be used to retrieve a mutable version of wire protocol
// messages.
func ReadFrom(io.Reader) (WireMessage, error) { return nil, nil }

// Unmarshal will unmarshal data into a WireMessage.
func Unmarshal([]byte) (WireMessage, error) { return nil, nil }

// Validate will validate that data is a valid MongoDB wire protocol message.
func Validate([]byte) error { return nil }
