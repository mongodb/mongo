package mongo

import (
	"context"

	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver/topology"
)

// batchCursor is the interface implemented by types that can provide batches of document results.
// The Cursor type is built on top of this type.
type batchCursor interface {
	// ID returns the ID of the cursor.
	ID() int64

	// Next returns true if there is a batch available.
	Next(context.Context) bool

	// Batch will return a DocumentSequence for the current batch of documents. The returned
	// DocumentSequence is only valid until the next call to Next or Close.
	Batch() *bsoncore.DocumentSequence

	// Server returns a pointer to the cursor's server.
	Server() *topology.Server

	// Err returns the last error encountered.
	Err() error

	// Close closes the cursor.
	Close(context.Context) error
}
