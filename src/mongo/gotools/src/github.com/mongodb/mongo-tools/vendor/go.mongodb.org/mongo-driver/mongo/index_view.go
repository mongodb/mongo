// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"bytes"
	"context"
	"errors"
	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/bson/bsontype"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
)

// ErrInvalidIndexValue indicates that the index Keys document has a value that isn't either a number or a string.
var ErrInvalidIndexValue = errors.New("invalid index value")

// ErrNonStringIndexName indicates that the index name specified in the options is not a string.
var ErrNonStringIndexName = errors.New("index name must be a string")

// ErrMultipleIndexDrop indicates that multiple indexes would be dropped from a call to IndexView.DropOne.
var ErrMultipleIndexDrop = errors.New("multiple indexes would be dropped")

// IndexView is used to create, drop, and list indexes on a given collection.
type IndexView struct {
	coll *Collection
}

// IndexModel contains information about an index.
type IndexModel struct {
	Keys    interface{}
	Options *options.IndexOptions
}

// List returns a cursor iterating over all the indexes in the collection.
func (iv IndexView) List(ctx context.Context, opts ...*options.ListIndexesOptions) (*Cursor, error) {
	sess := sessionFromContext(ctx)

	err := iv.coll.client.validSession(sess)
	if err != nil {
		return nil, err
	}

	listCmd := command.ListIndexes{
		NS:      iv.coll.namespace(),
		Session: sess,
		Clock:   iv.coll.client.clock,
	}

	readSelector := description.CompositeSelector([]description.ServerSelector{
		description.ReadPrefSelector(readpref.Primary()),
		description.LatencySelector(iv.coll.client.localThreshold),
	})
	batchCursor, err := driver.ListIndexes(
		ctx, listCmd,
		iv.coll.client.topology,
		readSelector,
		iv.coll.client.id,
		iv.coll.client.topology.SessionPool,
		opts...,
	)
	if err != nil {
		if err == command.ErrEmptyCursor {
			return newEmptyCursor(), nil
		}
		return nil, replaceErrors(err)
	}

	cursor, err := newCursor(batchCursor, iv.coll.registry)
	return cursor, replaceErrors(err)
}

// CreateOne creates a single index in the collection specified by the model.
func (iv IndexView) CreateOne(ctx context.Context, model IndexModel, opts ...*options.CreateIndexesOptions) (string, error) {
	names, err := iv.CreateMany(ctx, []IndexModel{model}, opts...)
	if err != nil {
		return "", err
	}

	return names[0], nil
}

// CreateMany creates multiple indexes in the collection specified by the models. The names of the
// created indexes are returned.
func (iv IndexView) CreateMany(ctx context.Context, models []IndexModel, opts ...*options.CreateIndexesOptions) ([]string, error) {
	names := make([]string, 0, len(models))
	indexes := bsonx.Arr{}

	for _, model := range models {
		if model.Keys == nil {
			return nil, fmt.Errorf("index model keys cannot be nil")
		}

		name, err := getOrGenerateIndexName(iv.coll.registry, model)
		if err != nil {
			return nil, err
		}

		names = append(names, name)

		keys, err := transformDocument(iv.coll.registry, model.Keys)
		if err != nil {
			return nil, err
		}
		index := bsonx.Doc{{"key", bsonx.Document(keys)}}
		if model.Options != nil {
			optsDoc, err := iv.createOptionsDoc(model.Options)
			if err != nil {
				return nil, err
			}

			index = append(index, optsDoc...)
		}
		index = index.Set("name", bsonx.String(name))

		indexes = append(indexes, bsonx.Document(index))
	}

	sess := sessionFromContext(ctx)

	err := iv.coll.client.validSession(sess)
	if err != nil {
		return nil, err
	}

	cmd := command.CreateIndexes{
		NS:      iv.coll.namespace(),
		Indexes: indexes,
		Session: sess,
		Clock:   iv.coll.client.clock,
	}

	_, err = driver.CreateIndexes(
		ctx, cmd,
		iv.coll.client.topology,
		iv.coll.writeSelector,
		iv.coll.client.id,
		iv.coll.client.topology.SessionPool,
		opts...,
	)
	if err != nil {
		return nil, err
	}

	return names, nil
}

func (iv IndexView) createOptionsDoc(opts *options.IndexOptions) (bsonx.Doc, error) {
	optsDoc := bsonx.Doc{}
	if opts.Background != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"background", bsonx.Boolean(*opts.Background)})
	}
	if opts.ExpireAfterSeconds != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"expireAfterSeconds", bsonx.Int32(*opts.ExpireAfterSeconds)})
	}
	if opts.Name != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"name", bsonx.String(*opts.Name)})
	}
	if opts.Sparse != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"sparse", bsonx.Boolean(*opts.Sparse)})
	}
	if opts.StorageEngine != nil {
		doc, err := transformDocument(iv.coll.registry, opts.StorageEngine)
		if err != nil {
			return nil, err
		}

		optsDoc = append(optsDoc, bsonx.Elem{"storageEngine", bsonx.Document(doc)})
	}
	if opts.Unique != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"unique", bsonx.Boolean(*opts.Unique)})
	}
	if opts.Version != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"v", bsonx.Int32(*opts.Version)})
	}
	if opts.DefaultLanguage != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"default_language", bsonx.String(*opts.DefaultLanguage)})
	}
	if opts.LanguageOverride != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"language_override", bsonx.String(*opts.LanguageOverride)})
	}
	if opts.TextVersion != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"textIndexVersion", bsonx.Int32(*opts.TextVersion)})
	}
	if opts.Weights != nil {
		weightsDoc, err := transformDocument(iv.coll.registry, opts.Weights)
		if err != nil {
			return nil, err
		}

		optsDoc = append(optsDoc, bsonx.Elem{"weights", bsonx.Document(weightsDoc)})
	}
	if opts.SphereVersion != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"2dsphereIndexVersion", bsonx.Int32(*opts.SphereVersion)})
	}
	if opts.Bits != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"bits", bsonx.Int32(*opts.Bits)})
	}
	if opts.Max != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"max", bsonx.Double(*opts.Max)})
	}
	if opts.Min != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"min", bsonx.Double(*opts.Min)})
	}
	if opts.BucketSize != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"bucketSize", bsonx.Int32(*opts.BucketSize)})
	}
	if opts.PartialFilterExpression != nil {
		doc, err := transformDocument(iv.coll.registry, opts.PartialFilterExpression)
		if err != nil {
			return nil, err
		}

		optsDoc = append(optsDoc, bsonx.Elem{"partialFilterExpression", bsonx.Document(doc)})
	}
	if opts.Collation != nil {
		collDoc, err := bsonx.ReadDoc(opts.Collation.ToDocument())
		if err != nil {
			return nil, err
		}
		optsDoc = append(optsDoc, bsonx.Elem{"collation", bsonx.Document(collDoc)})
	}

	return optsDoc, nil
}

// DropOne drops the index with the given name from the collection.
func (iv IndexView) DropOne(ctx context.Context, name string, opts ...*options.DropIndexesOptions) (bson.Raw, error) {
	if name == "*" {
		return nil, ErrMultipleIndexDrop
	}

	sess := sessionFromContext(ctx)

	err := iv.coll.client.validSession(sess)
	if err != nil {
		return nil, err
	}

	cmd := command.DropIndexes{
		NS:      iv.coll.namespace(),
		Index:   name,
		Session: sess,
		Clock:   iv.coll.client.clock,
	}

	return driver.DropIndexes(
		ctx, cmd,
		iv.coll.client.topology,
		iv.coll.writeSelector,
		iv.coll.client.id,
		iv.coll.client.topology.SessionPool,
		opts...,
	)
}

// DropAll drops all indexes in the collection.
func (iv IndexView) DropAll(ctx context.Context, opts ...*options.DropIndexesOptions) (bson.Raw, error) {
	sess := sessionFromContext(ctx)

	err := iv.coll.client.validSession(sess)
	if err != nil {
		return nil, err
	}

	cmd := command.DropIndexes{
		NS:      iv.coll.namespace(),
		Index:   "*",
		Session: sess,
		Clock:   iv.coll.client.clock,
	}

	return driver.DropIndexes(
		ctx, cmd,
		iv.coll.client.topology,
		iv.coll.writeSelector,
		iv.coll.client.id,
		iv.coll.client.topology.SessionPool,
		opts...,
	)
}

func getOrGenerateIndexName(registry *bsoncodec.Registry, model IndexModel) (string, error) {
	if model.Options != nil && model.Options.Name != nil {
		return *model.Options.Name, nil
	}

	name := bytes.NewBufferString("")
	first := true

	keys, err := transformDocument(registry, model.Keys)
	if err != nil {
		return "", err
	}
	for _, elem := range keys {
		if !first {
			_, err := name.WriteRune('_')
			if err != nil {
				return "", err
			}
		}

		_, err := name.WriteString(elem.Key)
		if err != nil {
			return "", err
		}

		_, err = name.WriteRune('_')
		if err != nil {
			return "", err
		}

		var value string

		switch elem.Value.Type() {
		case bsontype.Int32:
			value = fmt.Sprintf("%d", elem.Value.Int32())
		case bsontype.Int64:
			value = fmt.Sprintf("%d", elem.Value.Int64())
		case bsontype.String:
			value = elem.Value.StringValue()
		default:
			return "", ErrInvalidIndexValue
		}

		_, err = name.WriteString(value)
		if err != nil {
			return "", err
		}

		first = false
	}

	return name.String(), nil
}
