// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	"context"
	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo"
	mopt "go.mongodb.org/mongo-driver/mongo/options"
)

// BufferedBulkInserter implements a bufio.Writer-like design for queuing up
// documents and inserting them in bulk when the given doc limit (or max
// message size) is reached. Must be flushed at the end to ensure that all
// documents are written.
type BufferedBulkInserter struct {
	collection               *mongo.Collection
	documents                []interface{}
	docLimit                 int
	byteCount                int
	docCount                 int
	unordered                bool
	bypassDocumentValidation bool
}

// NewBufferedBulkInserter returns an initialized BufferedBulkInserter
// for writing.
func NewBufferedBulkInserter(collection *mongo.Collection, docLimit int, unordered bool) *BufferedBulkInserter {
	bb := &BufferedBulkInserter{
		collection:      collection,
		unordered:       unordered,
		docLimit:        docLimit,
	}
	bb.resetBulk()
	return bb
}

func (bb *BufferedBulkInserter) Unordered() {
	bb.unordered = true
}

func (bb *BufferedBulkInserter) SetBypassDocumentValidation(bypass bool) {
	bb.bypassDocumentValidation = bypass
}

// throw away the old bulk and init a new one
func (bb *BufferedBulkInserter) resetBulk() {
	bb.documents = make([]interface{}, 0, bb.docLimit)
	bb.byteCount = 0
	bb.docCount = 0
}

// Insert adds a document to the buffer for bulk insertion. If the buffer is
// full, the bulk insert is made, returning any error that occurs.
func (bb *BufferedBulkInserter) Insert(doc interface{}) error {
	rawBytes, err := bson.Marshal(doc)
	if err != nil {
		return fmt.Errorf("bson encoding error: %v", err)
	}

	return bb.InsertRaw(rawBytes)
}

// InsertRaw adds a document, represented as raw bson bytes, to the buffer for bulk insertion. If the buffer is full,
// the bulk insert is made, returning any error that occurs.
func (bb *BufferedBulkInserter) InsertRaw(rawBytes []byte) (err error) {
	// flush if we are full
	//
	// XXX With OP_MSG the limit is larger; MaxBSONSize shouldn't be hard
	// coded, it should be based on the server's ismaster response.
	if bb.docCount >= bb.docLimit || bb.byteCount+len(rawBytes) > MaxBSONSize {
		err = bb.Flush()
	}
	// buffer the document
	bb.docCount++
	bb.byteCount += len(rawBytes)
	bb.documents = append(bb.documents, bson.Raw(rawBytes))
	return err
}

// Flush writes all buffered documents in one bulk insert then resets the buffer.
func (bb *BufferedBulkInserter) Flush() error {
	if bb.docCount == 0 {
		return nil
	}
	defer bb.resetBulk()
	insertOpts := mopt.InsertMany().SetOrdered(!bb.unordered).SetBypassDocumentValidation(bb.bypassDocumentValidation)
	_, err := bb.collection.InsertMany(context.Background(), bb.documents, insertOpts)
	return err
}
