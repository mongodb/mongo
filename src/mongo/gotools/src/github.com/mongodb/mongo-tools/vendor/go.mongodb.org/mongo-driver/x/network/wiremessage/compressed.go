// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package wiremessage

import (
	"errors"
	"fmt"
)

// Compressed represents the OP_COMPRESSED message of the MongoDB wire protocol.
type Compressed struct {
	MsgHeader         Header
	OriginalOpCode    OpCode
	UncompressedSize  int32
	CompressorID      CompressorID
	CompressedMessage []byte
}

// MarshalWireMessage implements the Marshaler and WireMessage interfaces.
func (c Compressed) MarshalWireMessage() ([]byte, error) {
	b := make([]byte, 0, c.Len())
	return c.AppendWireMessage(b)
}

// ValidateWireMessage implements the Validator and WireMessage interfaces.
func (c Compressed) ValidateWireMessage() error {
	if int(c.MsgHeader.MessageLength) != c.Len() {
		return errors.New("incorrect header: message length is not correct")
	}

	if c.MsgHeader.OpCode != OpCompressed {
		return errors.New("incorrect header: opcode is not OpCompressed")
	}

	if c.OriginalOpCode != c.MsgHeader.OpCode {
		return errors.New("incorrect header: original opcode does not match opcode in message header")
	}
	return nil
}

// AppendWireMessage implements the Appender and WireMessage interfaces.
//
// AppendWireMessage will set the MessageLength property of MsgHeader if it is 0. It will also set the OpCode to
// OpCompressed if the OpCode is 0. If either of these properties are non-zero and not correct, this method will return
// both the []byte with the wire message appended to it and an invalid header error.
func (c Compressed) AppendWireMessage(b []byte) ([]byte, error) {
	err := c.MsgHeader.SetDefaults(c.Len(), OpCompressed)

	b = c.MsgHeader.AppendHeader(b)
	b = appendInt32(b, int32(c.OriginalOpCode))
	b = appendInt32(b, c.UncompressedSize)
	b = append(b, byte(c.CompressorID))
	b = append(b, c.CompressedMessage...)

	return b, err
}

// String implements the fmt.Stringer interface.
func (c Compressed) String() string {
	return fmt.Sprintf(
		`OP_COMPRESSED{MsgHeader: %s, Uncompressed Size: %d, CompressorId: %d, Compressed message: %s}`,
		c.MsgHeader, c.UncompressedSize, c.CompressorID, c.CompressedMessage,
	)
}

// Len implements the WireMessage interface.
func (c Compressed) Len() int {
	// Header + OpCode + UncompressedSize + CompressorId + CompressedMessage
	return 16 + 4 + 4 + 1 + len(c.CompressedMessage)
}

// UnmarshalWireMessage implements the Unmarshaler interface.
func (c *Compressed) UnmarshalWireMessage(b []byte) error {
	var err error
	c.MsgHeader, err = ReadHeader(b, 0)
	if err != nil {
		return err
	}

	if len(b) < int(c.MsgHeader.MessageLength) {
		return Error{Type: ErrOpCompressed, Message: "[]byte too small"}
	}

	c.OriginalOpCode = OpCode(readInt32(b, 16)) // skip first 16 for header
	c.UncompressedSize = readInt32(b, 20)
	c.CompressorID = CompressorID(b[24])

	// messageLength - Header - OpCode - UncompressedSize - CompressorId
	msgLen := c.MsgHeader.MessageLength - 16 - 4 - 4 - 1
	c.CompressedMessage = b[25 : 25+msgLen]

	return nil
}

// CompressorID is the ID for each type of Compressor.
type CompressorID uint8

// These constants represent the individual compressor IDs for an OP_COMPRESSED.
const (
	CompressorNoOp CompressorID = iota
	CompressorSnappy
	CompressorZLib
)

// DefaultZlibLevel is the default level for zlib compression
const DefaultZlibLevel = 6
