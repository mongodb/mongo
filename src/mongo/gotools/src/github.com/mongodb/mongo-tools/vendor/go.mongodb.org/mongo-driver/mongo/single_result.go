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
// returned.
func (sr *SingleResult) Decode(v interface{}) error {
	if sr.err != nil {
		return sr.err
	}
	if sr.reg == nil {
		return bson.ErrNilRegistry
	}
	switch {
	case sr.rdr != nil:
		if v == nil {
			return nil
		}
		return bson.UnmarshalWithRegistry(sr.reg, sr.rdr, v)
	case sr.cur != nil:
		defer sr.cur.Close(context.TODO())
		if !sr.cur.Next(context.TODO()) {
			if err := sr.cur.Err(); err != nil {
				return err
			}
			return ErrNoDocuments
		}
		if v == nil {
			return nil
		}
		return sr.cur.Decode(v)
	}

	return ErrNoDocuments
}

// DecodeBytes will return a copy of the document as a bson.Raw. If there was an
// error from the operation that created this SingleResult then the error
// will be returned. If there were no returned documents, ErrNoDocuments is
// returned.
func (sr *SingleResult) DecodeBytes() (bson.Raw, error) {
	switch {
	case sr.err != nil:
		return nil, sr.err
	case sr.rdr != nil:
		return sr.rdr, nil
	case sr.cur != nil:
		defer sr.cur.Close(context.TODO())
		if !sr.cur.Next(context.TODO()) {
			if err := sr.cur.Err(); err != nil {
				return nil, err
			}
			return nil, ErrNoDocuments
		}
		return sr.cur.Current, nil
	}

	return nil, ErrNoDocuments
}

// Err will return the error from the operation that created this SingleResult.
// If there was no error, nil is returned.
func (sr *SingleResult) Err() error {
	return sr.err
}
