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

// Count issues a count command. We don't use the Hint because
// that's not supported with older servers.
func (q *DeferredQuery) Count() (int, error) {
	opt := mopt.Count()
	filter := q.Filter
	if filter == nil {
		filter = bson.D{}
	}
	c, err := q.Coll.CountDocuments(nil, filter, opt)
	return int(c), err
}

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
