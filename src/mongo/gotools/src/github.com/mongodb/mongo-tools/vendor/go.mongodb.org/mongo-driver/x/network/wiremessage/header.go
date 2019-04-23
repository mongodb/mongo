// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package wiremessage

import (
	"fmt"
)

// ErrInvalidHeader is returned when methods are called on a malformed Header.
var ErrInvalidHeader error = Error{Type: ErrHeader, Message: "invalid header"}

// ErrHeaderTooSmall is returned when the size of the header is too small to be valid.
var ErrHeaderTooSmall error = Error{Type: ErrHeader, Message: "the header is too small to be valid"}

// ErrHeaderTooFewBytes is returned when a call to ReadHeader does not contain enough
// bytes to be a valid header.
var ErrHeaderTooFewBytes error = Error{Type: ErrHeader, Message: "invalid header because []byte too small"}

// ErrHeaderInvalidLength is returned when the MessageLength of a header is
// set but is not set to the correct size.
var ErrHeaderInvalidLength error = Error{Type: ErrHeader, Message: "invalid header because MessageLength is imporperly set"}

// ErrHeaderIncorrectOpCode is returned when the OpCode on a header is set but
// is not set to the correct OpCode.
var ErrHeaderIncorrectOpCode error = Error{Type: ErrHeader, Message: "invalid header because OpCode is improperly set"}

// Header represents the header of a MongoDB wire protocol message.
type Header struct {
	MessageLength int32
	RequestID     int32
	ResponseTo    int32
	OpCode        OpCode
}

// ReadHeader reads a header from the given slice of bytes starting at offset
// pos.
func ReadHeader(b []byte, pos int32) (Header, error) {
	if len(b) < 16 {
		return Header{}, ErrHeaderTooFewBytes
	}
	return Header{
		MessageLength: readInt32(b, 0),
		RequestID:     readInt32(b, 4),
		ResponseTo:    readInt32(b, 8),
		OpCode:        OpCode(readInt32(b, 12)),
	}, nil
}

func (h Header) String() string {
	return fmt.Sprintf(
		`Header{MessageLength: %d, RequestID: %d, ResponseTo: %d, OpCode: %v}`,
		h.MessageLength, h.RequestID, h.ResponseTo, h.OpCode,
	)
}

// AppendHeader will append this header to the given slice of bytes.
func (h Header) AppendHeader(b []byte) []byte {
	b = appendInt32(b, h.MessageLength)
	b = appendInt32(b, h.RequestID)
	b = appendInt32(b, h.ResponseTo)
	b = appendInt32(b, int32(h.OpCode))

	return b
}

// SetDefaults sets the length and opcode of this header.
func (h *Header) SetDefaults(length int, opcode OpCode) error {
	switch h.MessageLength {
	case int32(length):
	case 0:
		h.MessageLength = int32(length)
	default:
		return ErrHeaderInvalidLength
	}
	switch h.OpCode {
	case opcode:
	case OpCode(0):
		h.OpCode = opcode
	default:
		return ErrHeaderIncorrectOpCode
	}
	return nil
}
