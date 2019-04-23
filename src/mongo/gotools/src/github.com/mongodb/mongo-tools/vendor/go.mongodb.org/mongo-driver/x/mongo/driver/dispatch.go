// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package driver // import "go.mongodb.org/mongo-driver/x/mongo/driver"

import (
	"errors"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
)

// ErrCollation is caused if a collation is given for an invalid server version.
var ErrCollation = errors.New("collation cannot be set for server versions < 3.4")

// ErrArrayFilters is caused if array filters are given for an invalid server version.
var ErrArrayFilters = errors.New("array filters cannot be set for server versions < 3.6")

func interfaceToDocument(val interface{}, registry *bsoncodec.Registry) (bsonx.Doc, error) {
	if val == nil {
		return bsonx.Doc{}, nil
	}

	if registry == nil {
		registry = bson.DefaultRegistry
	}

	if bs, ok := val.([]byte); ok {
		// Slight optimization so we'll just use MarshalBSON and not go through the codec machinery.
		val = bson.Raw(bs)
	}

	// TODO(skriptble): Use a pool of these instead.
	buf := make([]byte, 0, 256)
	b, err := bson.MarshalAppendWithRegistry(registry, buf, val)
	if err != nil {
		return nil, err
	}
	return bsonx.ReadDoc(b)
}

func interfaceToElement(key string, i interface{}, registry *bsoncodec.Registry) (bsonx.Elem, error) {
	switch conv := i.(type) {
	case string:
		return bsonx.Elem{key, bsonx.String(conv)}, nil
	case bsonx.Doc:
		return bsonx.Elem{key, bsonx.Document(conv)}, nil
	default:
		doc, err := interfaceToDocument(i, registry)
		if err != nil {
			return bsonx.Elem{}, err
		}

		return bsonx.Elem{key, bsonx.Document(doc)}, nil
	}
}

func closeImplicitSession(sess *session.Client) {
	if sess != nil && sess.SessionType == session.Implicit {
		sess.EndSession()
	}
}
