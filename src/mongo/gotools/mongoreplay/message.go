// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"fmt"
	"io"
)

// MsgHeaderLen is the message header length in bytes
const MsgHeaderLen = 16

// MaxMessageSize is the maximum message size as defined in the server
const MaxMessageSize = 48 * 1000 * 1000

// MsgHeader is the mongo MessageHeader
type MsgHeader struct {
	// MessageLength is the total message size, including this header
	MessageLength int32
	// RequestID is the identifier for this miessage
	RequestID int32
	// ResponseTo is the RequestID of the message being responded to;
	// used in DB responses
	ResponseTo int32
	// OpCode is the request type, see consts above.
	OpCode OpCode
}

// ReadHeader creates a new MsgHeader given a reader at the beginning of a
// message.
func ReadHeader(r io.Reader) (*MsgHeader, error) {
	var d [MsgHeaderLen]byte
	b := d[:]
	if _, err := io.ReadFull(r, b); err != nil {
		return nil, err
	}
	h := MsgHeader{}
	h.FromWire(b)
	return &h, nil
}

// ToWire converts the MsgHeader to the wire protocol
func (m MsgHeader) ToWire() []byte {
	var d [MsgHeaderLen]byte
	b := d[:]
	SetInt32(b, 0, m.MessageLength)
	SetInt32(b, 4, m.RequestID)
	SetInt32(b, 8, m.ResponseTo)
	SetInt32(b, 12, int32(m.OpCode))
	return b
}

// FromWire reads the wirebytes into this object
func (m *MsgHeader) FromWire(b []byte) {
	m.MessageLength = getInt32(b, 0)
	m.RequestID = getInt32(b, 4)
	m.ResponseTo = getInt32(b, 8)
	m.OpCode = OpCode(getInt32(b, 12))
}

// WriteTo writes the MsgHeader into a writer.
func (m *MsgHeader) WriteTo(w io.Writer) (int64, error) {
	b := m.ToWire()
	c, err := w.Write(b)
	n := int64(c)
	if err != nil {
		return n, err
	}
	if c != len(b) {
		return n, fmt.Errorf("attempted to write %d but wrote %d", len(b), n)
	}
	return n, nil
}

// LooksReal does a best efffort to detect if a MsgHeadr is not invalid
func (m *MsgHeader) LooksReal() bool {
	// AFAIK, the smallest wire protocol message possible is a 24 byte
	// KILL_CURSORS_OP
	if m.MessageLength > MaxMessageSize || m.MessageLength < 24 {
		return false
	}
	if m.RequestID < 0 {
		return false
	}
	if m.ResponseTo < 0 {
		return false
	}
	return goodOpCode[int32(m.OpCode)]
}

// String returns a string representation of the message header.
// Useful for debugging.
func (m *MsgHeader) String() string {
	return fmt.Sprintf(
		"opCode:%s (%d) msgLen:%d reqID:%d respID:%d",
		m.OpCode,
		m.OpCode,
		m.MessageLength,
		m.RequestID,
		m.ResponseTo,
	)
}
