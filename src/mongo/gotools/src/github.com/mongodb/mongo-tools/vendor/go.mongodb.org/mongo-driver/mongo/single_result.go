// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"context"
	"errors"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsoncodec"
)

// ErrNoDocuments is returned by Decode when an operation that returns a
// SingleResult doesn't return any documents.
var ErrNoDocuments = errors.New("mongo: no documents in result")

// SingleResult represents a single document returned from an operation. If
// the operation returned an error, the Err method of SingleResult will
// return that error.
type SingleResult struct {
	err error
	cur *Cursor
	rdr bson.Raw
	reg *bsoncodec.Registry
}

// Decode will attempt to decode the first document into v. If there was an
// error from the operation that created this SingleResult then the error
// will be returned. If there were no returned documents, ErrNoDocuments is
// returned. If v is nil or is a typed nil, an error will be returned.
func (sr *SingleResult) Decode(v interface{}) error {
	if sr.err != nil {
		return sr.err
	}
	if sr.reg == nil {
		return bson.ErrNilRegistry
	}

	if sr.err = sr.setRdrContents(); sr.err != nil {
		return sr.err
	}
	return bson.UnmarshalWithRegistry(sr.reg, sr.rdr, v)
}

// DecodeBytes will return a copy of the document as a bson.Raw. If there was an
// error from the operation that created this SingleResult then the error
// will be returned along with the result. If there were no returned documents, ErrNoDocuments is
// returned.
func (sr *SingleResult) DecodeBytes() (bson.Raw, error) {
	if sr.err != nil {
		return sr.rdr, sr.err
	}

	if sr.err = sr.setRdrContents(); sr.err != nil {
		return nil, sr.err
	}
	return sr.rdr, nil
}

// setRdrContents will set the contents of rdr by iterating the underlying cursor if necessary.
func (sr *SingleResult) setRdrContents() error {
	switch {
	case sr.err != nil:
		return sr.err
	case sr.rdr != nil:
		return nil
	case sr.cur != nil:
		defer sr.cur.Close(context.TODO())
		if !sr.cur.Next(context.TODO()) {
			if err := sr.cur.Err(); err != nil {
				return err
			}

			return ErrNoDocuments
		}
		sr.rdr = sr.cur.Current
		return nil
	}

	return ErrNoDocuments
}

// Err will return the error from the operation that created this SingleResult.
// If there was no error, nil is returned.
func (sr *SingleResult) Err() error {
	sr.err = sr.setRdrContents()

	return sr.err
}
