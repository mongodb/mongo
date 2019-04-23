// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package wiremessage

import (
	"bytes"
	"errors"

	"go.mongodb.org/mongo-driver/bson"
)

func readInt32(b []byte, pos int32) int32 {
	return (int32(b[pos+0])) | (int32(b[pos+1]) << 8) | (int32(b[pos+2]) << 16) | (int32(b[pos+3]) << 24)
}

func readCString(b []byte, pos int32) (string, error) {
	null := bytes.IndexByte(b[pos:], 0x00)
	if null == -1 {
		return "", errors.New("invalid cstring")
	}
	return string(b[pos : int(pos)+null]), nil
}

func readInt64(b []byte, pos int32) int64 {
	return (int64(b[pos+0])) | (int64(b[pos+1]) << 8) | (int64(b[pos+2]) << 16) | (int64(b[pos+3]) << 24) | (int64(b[pos+4]) << 32) |
		(int64(b[pos+5]) << 40) | (int64(b[pos+6]) << 48) | (int64(b[pos+7]) << 56)

}

// readDocument will attempt to read a bson.Reader from the given slice of bytes
// from the given position.
func readDocument(b []byte, pos int32) (bson.Raw, int, Error) {
	if int(pos)+4 > len(b) {
		return nil, 0, Error{Message: "document too small to be valid"}
	}
	size := int(readInt32(b, int32(pos)))
	if int(pos)+size > len(b) {
		return nil, 0, Error{Message: "document size is larger than available bytes"}
	}
	if b[int(pos)+size-1] != 0x00 {
		return nil, 0, Error{Message: "document invalid, last byte is not null"}
	}
	// TODO(GODRIVER-138): When we add 3.0 support, alter this so we either do one larger make or use a pool.
	rdr := make(bson.Raw, size)
	copy(rdr, b[pos:int(pos)+size])
	return rdr, size, Error{Type: ErrNil}
}
