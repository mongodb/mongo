package db

import (
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo"
	mopt "go.mongodb.org/mongo-driver/mongo/options"
)

// DeferredQuery represents a deferred query
type DeferredQuery struct {
	Coll      *mongo.Collection
	Filter    interface{}
	Hint      interface{}
	LogReplay bool
}

// EstimatedDocumentCount issues a count command.
func (q *DeferredQuery) EstimatedDocumentCount() (int, error) {
	opt := mopt.EstimatedDocumentCount()
	c, err := q.Coll.EstimatedDocumentCount(nil, opt)
	return int(c), err
}

// Iter executes a find query and returns a cursor.
func (q *DeferredQuery) Iter() (*mongo.Cursor, error) {
	opts := mopt.Find()
	if q.Hint != nil {
		opts.SetHint(q.Hint)
	}
	if q.LogReplay {
		opts.SetOplogReplay(true)
	}
	filter := q.Filter
	if filter == nil {
		filter = bson.D{}
	}
	return q.Coll.Find(nil, filter, opts)
}
