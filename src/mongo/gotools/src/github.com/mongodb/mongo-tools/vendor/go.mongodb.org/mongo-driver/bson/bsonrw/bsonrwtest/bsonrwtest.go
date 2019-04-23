// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package bsonrwtest // import "go.mongodb.org/mongo-driver/bson/bsonrw/bsonrwtest"

import (
	"testing"

	"go.mongodb.org/mongo-driver/bson/bsonrw"
	"go.mongodb.org/mongo-driver/bson/bsontype"
	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
)

var _ bsonrw.ValueReader = (*ValueReaderWriter)(nil)
var _ bsonrw.ValueWriter = (*ValueReaderWriter)(nil)

// Invoked is a type used to indicate what method was called last.
type Invoked byte

// These are the different methods that can be invoked.
const (
	Nothing Invoked = iota
	ReadArray
	ReadBinary
	ReadBoolean
	ReadDocument
	ReadCodeWithScope
	ReadDBPointer
	ReadDateTime
	ReadDecimal128
	ReadDouble
	ReadInt32
	ReadInt64
	ReadJavascript
	ReadMaxKey
	ReadMinKey
	ReadNull
	ReadObjectID
	ReadRegex
	ReadString
	ReadSymbol
	ReadTimestamp
	ReadUndefined
	ReadElement
	ReadValue
	WriteArray
	WriteBinary
	WriteBinaryWithSubtype
	WriteBoolean
	WriteCodeWithScope
	WriteDBPointer
	WriteDateTime
	WriteDecimal128
	WriteDouble
	WriteInt32
	WriteInt64
	WriteJavascript
	WriteMaxKey
	WriteMinKey
	WriteNull
	WriteObjectID
	WriteRegex
	WriteString
	WriteDocument
	WriteSymbol
	WriteTimestamp
	WriteUndefined
	WriteDocumentElement
	WriteDocumentEnd
	WriteArrayElement
	WriteArrayEnd
	Skip
)

func (i Invoked) String() string {
	switch i {
	case Nothing:
		return "Nothing"
	case ReadArray:
		return "ReadArray"
	case ReadBinary:
		return "ReadBinary"
	case ReadBoolean:
		return "ReadBoolean"
	case ReadDocument:
		return "ReadDocument"
	case ReadCodeWithScope:
		return "ReadCodeWithScope"
	case ReadDBPointer:
		return "ReadDBPointer"
	case ReadDateTime:
		return "ReadDateTime"
	case ReadDecimal128:
		return "ReadDecimal128"
	case ReadDouble:
		return "ReadDouble"
	case ReadInt32:
		return "ReadInt32"
	case ReadInt64:
		return "ReadInt64"
	case ReadJavascript:
		return "ReadJavascript"
	case ReadMaxKey:
		return "ReadMaxKey"
	case ReadMinKey:
		return "ReadMinKey"
	case ReadNull:
		return "ReadNull"
	case ReadObjectID:
		return "ReadObjectID"
	case ReadRegex:
		return "ReadRegex"
	case ReadString:
		return "ReadString"
	case ReadSymbol:
		return "ReadSymbol"
	case ReadTimestamp:
		return "ReadTimestamp"
	case ReadUndefined:
		return "ReadUndefined"
	case ReadElement:
		return "ReadElement"
	case ReadValue:
		return "ReadValue"
	case WriteArray:
		return "WriteArray"
	case WriteBinary:
		return "WriteBinary"
	case WriteBinaryWithSubtype:
		return "WriteBinaryWithSubtype"
	case WriteBoolean:
		return "WriteBoolean"
	case WriteCodeWithScope:
		return "WriteCodeWithScope"
	case WriteDBPointer:
		return "WriteDBPointer"
	case WriteDateTime:
		return "WriteDateTime"
	case WriteDecimal128:
		return "WriteDecimal128"
	case WriteDouble:
		return "WriteDouble"
	case WriteInt32:
		return "WriteInt32"
	case WriteInt64:
		return "WriteInt64"
	case WriteJavascript:
		return "WriteJavascript"
	case WriteMaxKey:
		return "WriteMaxKey"
	case WriteMinKey:
		return "WriteMinKey"
	case WriteNull:
		return "WriteNull"
	case WriteObjectID:
		return "WriteObjectID"
	case WriteRegex:
		return "WriteRegex"
	case WriteString:
		return "WriteString"
	case WriteDocument:
		return "WriteDocument"
	case WriteSymbol:
		return "WriteSymbol"
	case WriteTimestamp:
		return "WriteTimestamp"
	case WriteUndefined:
		return "WriteUndefined"
	case WriteDocumentElement:
		return "WriteDocumentElement"
	case WriteDocumentEnd:
		return "WriteDocumentEnd"
	case WriteArrayElement:
		return "WriteArrayElement"
	case WriteArrayEnd:
		return "WriteArrayEnd"
	default:
		return "<unknown>"
	}
}

// ValueReaderWriter is a test implementation of a bsonrw.ValueReader and bsonrw.ValueWriter
type ValueReaderWriter struct {
	T        *testing.T
	Invoked  Invoked
	Return   interface{} // Can be a primitive or a bsoncore.Value
	BSONType bsontype.Type
	Err      error
	ErrAfter Invoked // error after this method is called
	depth    uint64
}

// prevent infinite recursion.
func (llvrw *ValueReaderWriter) checkdepth() {
	llvrw.depth++
	if llvrw.depth > 1000 {
		panic("max depth exceeded")
	}
}

// Type implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) Type() bsontype.Type {
	llvrw.checkdepth()
	return llvrw.BSONType
}

// Skip implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) Skip() error {
	llvrw.checkdepth()
	llvrw.Invoked = Skip
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// ReadArray implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadArray() (bsonrw.ArrayReader, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadArray
	if llvrw.ErrAfter == llvrw.Invoked {
		return nil, llvrw.Err
	}

	return llvrw, nil
}

// ReadBinary implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadBinary() (b []byte, btype byte, err error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadBinary
	if llvrw.ErrAfter == llvrw.Invoked {
		return nil, 0x00, llvrw.Err
	}

	switch tt := llvrw.Return.(type) {
	case bsoncore.Value:
		subtype, data, _, ok := bsoncore.ReadBinary(tt.Data)
		if !ok {
			llvrw.T.Error("Invalid Value provided for return value of ReadBinary.")
			return nil, 0x00, nil
		}
		return data, subtype, nil
	default:
		llvrw.T.Errorf("Incorrect type provided for return value of ReadBinary: %T", llvrw.Return)
		return nil, 0x00, nil
	}
}

// ReadBoolean implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadBoolean() (bool, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadBoolean
	if llvrw.ErrAfter == llvrw.Invoked {
		return false, llvrw.Err
	}

	switch tt := llvrw.Return.(type) {
	case bool:
		return tt, nil
	case bsoncore.Value:
		b, _, ok := bsoncore.ReadBoolean(tt.Data)
		if !ok {
			llvrw.T.Error("Invalid Value provided for return value of ReadBoolean.")
			return false, nil
		}
		return b, nil
	default:
		llvrw.T.Errorf("Incorrect type provided for return value of ReadBoolean: %T", llvrw.Return)
		return false, nil
	}
}

// ReadDocument implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadDocument() (bsonrw.DocumentReader, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadDocument
	if llvrw.ErrAfter == llvrw.Invoked {
		return nil, llvrw.Err
	}

	return llvrw, nil
}

// ReadCodeWithScope implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadCodeWithScope() (code string, dr bsonrw.DocumentReader, err error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadCodeWithScope
	if llvrw.ErrAfter == llvrw.Invoked {
		return "", nil, llvrw.Err
	}

	return "", llvrw, nil
}

// ReadDBPointer implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadDBPointer() (ns string, oid primitive.ObjectID, err error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadDBPointer
	if llvrw.ErrAfter == llvrw.Invoked {
		return "", primitive.ObjectID{}, llvrw.Err
	}

	switch tt := llvrw.Return.(type) {
	case bsoncore.Value:
		ns, oid, _, ok := bsoncore.ReadDBPointer(tt.Data)
		if !ok {
			llvrw.T.Error("Invalid Value instance provided for return value of ReadDBPointer")
			return "", primitive.ObjectID{}, nil
		}
		return ns, oid, nil
	default:
		llvrw.T.Errorf("Incorrect type provided for return value of ReadDBPointer: %T", llvrw.Return)
		return "", primitive.ObjectID{}, nil
	}
}

// ReadDateTime implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadDateTime() (int64, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadDateTime
	if llvrw.ErrAfter == llvrw.Invoked {
		return 0, llvrw.Err
	}

	dt, ok := llvrw.Return.(int64)
	if !ok {
		llvrw.T.Errorf("Incorrect type provided for return value of ReadDateTime: %T", llvrw.Return)
		return 0, nil
	}

	return dt, nil
}

// ReadDecimal128 implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadDecimal128() (primitive.Decimal128, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadDecimal128
	if llvrw.ErrAfter == llvrw.Invoked {
		return primitive.Decimal128{}, llvrw.Err
	}

	d128, ok := llvrw.Return.(primitive.Decimal128)
	if !ok {
		llvrw.T.Errorf("Incorrect type provided for return value of ReadDecimal128: %T", llvrw.Return)
		return primitive.Decimal128{}, nil
	}

	return d128, nil
}

// ReadDouble implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadDouble() (float64, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadDouble
	if llvrw.ErrAfter == llvrw.Invoked {
		return 0, llvrw.Err
	}

	f64, ok := llvrw.Return.(float64)
	if !ok {
		llvrw.T.Errorf("Incorrect type provided for return value of ReadDouble: %T", llvrw.Return)
		return 0, nil
	}

	return f64, nil
}

// ReadInt32 implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadInt32() (int32, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadInt32
	if llvrw.ErrAfter == llvrw.Invoked {
		return 0, llvrw.Err
	}

	i32, ok := llvrw.Return.(int32)
	if !ok {
		llvrw.T.Errorf("Incorrect type provided for return value of ReadInt32: %T", llvrw.Return)
		return 0, nil
	}

	return i32, nil
}

// ReadInt64 implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadInt64() (int64, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadInt64
	if llvrw.ErrAfter == llvrw.Invoked {
		return 0, llvrw.Err
	}
	i64, ok := llvrw.Return.(int64)
	if !ok {
		llvrw.T.Errorf("Incorrect type provided for return value of ReadInt64: %T", llvrw.Return)
		return 0, nil
	}

	return i64, nil
}

// ReadJavascript implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadJavascript() (code string, err error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadJavascript
	if llvrw.ErrAfter == llvrw.Invoked {
		return "", llvrw.Err
	}
	js, ok := llvrw.Return.(string)
	if !ok {
		llvrw.T.Errorf("Incorrect type provided for return value of ReadJavascript: %T", llvrw.Return)
		return "", nil
	}

	return js, nil
}

// ReadMaxKey implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadMaxKey() error {
	llvrw.checkdepth()
	llvrw.Invoked = ReadMaxKey
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}

	return nil
}

// ReadMinKey implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadMinKey() error {
	llvrw.checkdepth()
	llvrw.Invoked = ReadMinKey
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}

	return nil
}

// ReadNull implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadNull() error {
	llvrw.checkdepth()
	llvrw.Invoked = ReadNull
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}

	return nil
}

// ReadObjectID implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadObjectID() (primitive.ObjectID, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadObjectID
	if llvrw.ErrAfter == llvrw.Invoked {
		return primitive.ObjectID{}, llvrw.Err
	}
	oid, ok := llvrw.Return.(primitive.ObjectID)
	if !ok {
		llvrw.T.Errorf("Incorrect type provided for return value of ReadObjectID: %T", llvrw.Return)
		return primitive.ObjectID{}, nil
	}

	return oid, nil
}

// ReadRegex implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadRegex() (pattern string, options string, err error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadRegex
	if llvrw.ErrAfter == llvrw.Invoked {
		return "", "", llvrw.Err
	}
	switch tt := llvrw.Return.(type) {
	case bsoncore.Value:
		pattern, options, _, ok := bsoncore.ReadRegex(tt.Data)
		if !ok {
			llvrw.T.Error("Invalid Value instance provided for ReadRegex")
			return "", "", nil
		}
		return pattern, options, nil
	default:
		llvrw.T.Errorf("Incorrect type provided for return value of ReadRegex: %T", llvrw.Return)
		return "", "", nil
	}
}

// ReadString implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadString() (string, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadString
	if llvrw.ErrAfter == llvrw.Invoked {
		return "", llvrw.Err
	}
	str, ok := llvrw.Return.(string)
	if !ok {
		llvrw.T.Errorf("Incorrect type provided for return value of ReadString: %T", llvrw.Return)
		return "", nil
	}

	return str, nil
}

// ReadSymbol implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadSymbol() (symbol string, err error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadSymbol
	if llvrw.ErrAfter == llvrw.Invoked {
		return "", llvrw.Err
	}
	switch tt := llvrw.Return.(type) {
	case string:
		return tt, nil
	case bsoncore.Value:
		symbol, _, ok := bsoncore.ReadSymbol(tt.Data)
		if !ok {
			llvrw.T.Error("Invalid Value instance provided for ReadSymbol")
			return "", nil
		}
		return symbol, nil
	default:
		llvrw.T.Errorf("Incorrect type provided for return value of ReadSymbol: %T", llvrw.Return)
		return "", nil
	}
}

// ReadTimestamp implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadTimestamp() (t uint32, i uint32, err error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadTimestamp
	if llvrw.ErrAfter == llvrw.Invoked {
		return 0, 0, llvrw.Err
	}
	switch tt := llvrw.Return.(type) {
	case bsoncore.Value:
		t, i, _, ok := bsoncore.ReadTimestamp(tt.Data)
		if !ok {
			llvrw.T.Errorf("Invalid Value instance provided for return value of ReadTimestamp")
			return 0, 0, nil
		}
		return t, i, nil
	default:
		llvrw.T.Errorf("Incorrect type provided for return value of ReadTimestamp: %T", llvrw.Return)
		return 0, 0, nil
	}
}

// ReadUndefined implements the bsonrw.ValueReader interface.
func (llvrw *ValueReaderWriter) ReadUndefined() error {
	llvrw.checkdepth()
	llvrw.Invoked = ReadUndefined
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}

	return nil
}

// WriteArray implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteArray() (bsonrw.ArrayWriter, error) {
	llvrw.checkdepth()
	llvrw.Invoked = WriteArray
	if llvrw.ErrAfter == llvrw.Invoked {
		return nil, llvrw.Err
	}
	return llvrw, nil
}

// WriteBinary implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteBinary(b []byte) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteBinary
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteBinaryWithSubtype implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteBinaryWithSubtype(b []byte, btype byte) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteBinaryWithSubtype
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteBoolean implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteBoolean(bool) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteBoolean
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteCodeWithScope implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteCodeWithScope(code string) (bsonrw.DocumentWriter, error) {
	llvrw.checkdepth()
	llvrw.Invoked = WriteCodeWithScope
	if llvrw.ErrAfter == llvrw.Invoked {
		return nil, llvrw.Err
	}
	return llvrw, nil
}

// WriteDBPointer implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteDBPointer(ns string, oid primitive.ObjectID) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteDBPointer
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteDateTime implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteDateTime(dt int64) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteDateTime
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteDecimal128 implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteDecimal128(primitive.Decimal128) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteDecimal128
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteDouble implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteDouble(float64) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteDouble
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteInt32 implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteInt32(int32) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteInt32
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteInt64 implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteInt64(int64) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteInt64
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteJavascript implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteJavascript(code string) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteJavascript
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteMaxKey implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteMaxKey() error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteMaxKey
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteMinKey implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteMinKey() error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteMinKey
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteNull implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteNull() error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteNull
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteObjectID implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteObjectID(primitive.ObjectID) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteObjectID
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteRegex implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteRegex(pattern string, options string) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteRegex
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteString implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteString(string) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteString
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteDocument implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteDocument() (bsonrw.DocumentWriter, error) {
	llvrw.checkdepth()
	llvrw.Invoked = WriteDocument
	if llvrw.ErrAfter == llvrw.Invoked {
		return nil, llvrw.Err
	}
	return llvrw, nil
}

// WriteSymbol implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteSymbol(symbol string) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteSymbol
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteTimestamp implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteTimestamp(t uint32, i uint32) error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteTimestamp
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// WriteUndefined implements the bsonrw.ValueWriter interface.
func (llvrw *ValueReaderWriter) WriteUndefined() error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteUndefined
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}
	return nil
}

// ReadElement implements the bsonrw.DocumentReader interface.
func (llvrw *ValueReaderWriter) ReadElement() (string, bsonrw.ValueReader, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadElement
	if llvrw.ErrAfter == llvrw.Invoked {
		return "", nil, llvrw.Err
	}

	return "", llvrw, nil
}

// WriteDocumentElement implements the bsonrw.DocumentWriter interface.
func (llvrw *ValueReaderWriter) WriteDocumentElement(string) (bsonrw.ValueWriter, error) {
	llvrw.checkdepth()
	llvrw.Invoked = WriteDocumentElement
	if llvrw.ErrAfter == llvrw.Invoked {
		return nil, llvrw.Err
	}

	return llvrw, nil
}

// WriteDocumentEnd implements the bsonrw.DocumentWriter interface.
func (llvrw *ValueReaderWriter) WriteDocumentEnd() error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteDocumentEnd
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}

	return nil
}

// ReadValue implements the bsonrw.ArrayReader interface.
func (llvrw *ValueReaderWriter) ReadValue() (bsonrw.ValueReader, error) {
	llvrw.checkdepth()
	llvrw.Invoked = ReadValue
	if llvrw.ErrAfter == llvrw.Invoked {
		return nil, llvrw.Err
	}

	return llvrw, nil
}

// WriteArrayElement implements the bsonrw.ArrayWriter interface.
func (llvrw *ValueReaderWriter) WriteArrayElement() (bsonrw.ValueWriter, error) {
	llvrw.checkdepth()
	llvrw.Invoked = WriteArrayElement
	if llvrw.ErrAfter == llvrw.Invoked {
		return nil, llvrw.Err
	}

	return llvrw, nil
}

// WriteArrayEnd implements the bsonrw.ArrayWriter interface.
func (llvrw *ValueReaderWriter) WriteArrayEnd() error {
	llvrw.checkdepth()
	llvrw.Invoked = WriteArrayEnd
	if llvrw.ErrAfter == llvrw.Invoked {
		return llvrw.Err
	}

	return nil
}
