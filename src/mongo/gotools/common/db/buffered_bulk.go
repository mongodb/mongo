package db

import (
	"fmt"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

// BufferedBulkInserter implements a bufio.Writer-like design for queuing up
// documents and inserting them in bulk when the given doc limit (or max
// message size) is reached. Must be flushed at the end to ensure that all
// documents are written.
type BufferedBulkInserter struct {
	bulk            *mgo.Bulk
	collection      *mgo.Collection
	continueOnError bool
	docLimit        int
	byteCount       int
	docCount        int
	unordered       bool
}

// NewBufferedBulkInserter returns an initialized BufferedBulkInserter
// for writing.
func NewBufferedBulkInserter(collection *mgo.Collection, docLimit int,
	continueOnError bool) *BufferedBulkInserter {
	bb := &BufferedBulkInserter{
		collection:      collection,
		continueOnError: continueOnError,
		docLimit:        docLimit,
	}
	bb.resetBulk()
	return bb
}

func (bb *BufferedBulkInserter) Unordered() {
	bb.unordered = true
	bb.bulk.Unordered()
}

// throw away the old bulk and init a new one
func (bb *BufferedBulkInserter) resetBulk() {
	bb.bulk = bb.collection.Bulk()
	if bb.continueOnError || bb.unordered {
		bb.bulk.Unordered()
	}
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
	// flush if we are full
	if bb.docCount >= bb.docLimit || bb.byteCount+len(rawBytes) > MaxBSONSize {
		err = bb.Flush()
	}
	// buffer the document
	bb.docCount++
	bb.byteCount += len(rawBytes)
	bb.bulk.Insert(bson.Raw{Data: rawBytes})
	return err
}

// Flush writes all buffered documents in one bulk insert then resets the buffer.
func (bb *BufferedBulkInserter) Flush() error {
	if bb.docCount == 0 {
		return nil
	}
	defer bb.resetBulk()
	if _, err := bb.bulk.Run(); err != nil {
		return err
	}
	return nil
}
