// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"context"
	"errors"
	"io"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
)

// Cursor is used to iterate a stream of documents. Each document is decoded into the result
// according to the rules of the bson package.
//
// A typical usage of the Cursor type would be:
//
//		var cur *Cursor
//		ctx := context.Background()
//		defer cur.Close(ctx)
//
// 		for cur.Next(ctx) {
//			elem := &bson.D{}
//			if err := cur.Decode(elem); err != nil {
// 				log.Fatal(err)
// 			}
//
// 			// do something with elem....
//		}
//
// 		if err := cur.Err(); err != nil {
//			log.Fatal(err)
//		}
//
type Cursor struct {
	// Current is the BSON bytes of the current document. This property is only valid until the next
	// call to Next or Close. If continued access is required to the bson.Raw, you must make a copy
	// of it.
	Current bson.Raw

	bc       batchCursor
	batch    *bsoncore.DocumentSequence
	registry *bsoncodec.Registry

	err error
}

func newCursor(bc batchCursor, registry *bsoncodec.Registry) (*Cursor, error) {
	if registry == nil {
		registry = bson.DefaultRegistry
	}
	if bc == nil {
		return nil, errors.New("batch cursor must not be nil")
	}
	return &Cursor{bc: bc, registry: registry}, nil
}

func newEmptyCursor() *Cursor {
	return &Cursor{bc: driver.NewEmptyBatchCursor()}
}

// ID returns the ID of this cursor.
func (c *Cursor) ID() int64 { return c.bc.ID() }

// Next gets the next result from this cursor. Returns true if there were no errors and the next
// result is available for decoding.
func (c *Cursor) Next(ctx context.Context) bool {
	if ctx == nil {
		ctx = context.Background()
	}
	doc, err := c.batch.Next()
	switch err {
	case nil:
		c.Current = bson.Raw(doc)
		return true
	case io.EOF: // Need to do a getMore
	default:
		c.err = err
		return false
	}

	// call the Next method in a loop until at least one document is returned in the next batch or
	// the context times out.
	for {
		// If we don't have a next batch
		if !c.bc.Next(ctx) {
			// Do we have an error? If so we return false.
			c.err = c.bc.Err()
			if c.err != nil {
				return false
			}
			// Is the cursor ID zero?
			if c.bc.ID() == 0 {
				return false
			}
			// empty batch, but cursor is still valid, so continue.
			continue
		}

		c.batch = c.bc.Batch()
		doc, err = c.batch.Next()
		switch err {
		case nil:
			c.Current = bson.Raw(doc)
			return true
		case io.EOF: // Empty batch so we continue
		default:
			c.err = err
			return false
		}
	}
}

// Decode will decode the current document into val.
func (c *Cursor) Decode(val interface{}) error {
	return bson.UnmarshalWithRegistry(c.registry, c.Current, val)
}

// Err returns the current error.
func (c *Cursor) Err() error { return c.err }

// Close closes this cursor.
func (c *Cursor) Close(ctx context.Context) error { return c.bc.Close(ctx) }
