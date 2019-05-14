// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo // import "go.mongodb.org/mongo-driver/mongo"

import (
	"context"
	"errors"
	"fmt"
	"net"
	"reflect"
	"strings"

	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/x/bsonx"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/bson/bsontype"
	"go.mongodb.org/mongo-driver/bson/primitive"
)

// Dialer is used to make network connections.
type Dialer interface {
	DialContext(ctx context.Context, network, address string) (net.Conn, error)
}

// BSONAppender is an interface implemented by types that can marshal a
// provided type into BSON bytes and append those bytes to the provided []byte.
// The AppendBSON can return a non-nil error and non-nil []byte. The AppendBSON
// method may also write incomplete BSON to the []byte.
type BSONAppender interface {
	AppendBSON([]byte, interface{}) ([]byte, error)
}

// BSONAppenderFunc is an adapter function that allows any function that
// satisfies the AppendBSON method signature to be used where a BSONAppender is
// used.
type BSONAppenderFunc func([]byte, interface{}) ([]byte, error)

// AppendBSON implements the BSONAppender interface
func (baf BSONAppenderFunc) AppendBSON(dst []byte, val interface{}) ([]byte, error) {
	return baf(dst, val)
}

// MarshalError is returned when attempting to transform a value into a document
// results in an error.
type MarshalError struct {
	Value interface{}
	Err   error
}

// Error implements the error interface.
func (me MarshalError) Error() string {
	return fmt.Sprintf("cannot transform type %s to a BSON Document: %v", reflect.TypeOf(me.Value), me.Err)
}

// Pipeline is a type that makes creating aggregation pipelines easier. It is a
// helper and is intended for serializing to BSON.
//
// Example usage:
//
//		mongo.Pipeline{
//			{{"$group", bson.D{{"_id", "$state"}, {"totalPop", bson.D{{"$sum", "$pop"}}}}}},
//			{{"$match", bson.D{{"totalPop", bson.D{{"$gte", 10*1000*1000}}}}}},
//		}
//
type Pipeline []bson.D

// transformAndEnsureID is a hack that makes it easy to get a RawValue as the _id value. This will
// be removed when we switch from using bsonx to bsoncore for the driver package.
func transformAndEnsureID(registry *bsoncodec.Registry, val interface{}) (bsonx.Doc, interface{}, error) {
	// TODO: performance is going to be pretty bad for bsonx.Doc here since we turn it into a []byte
	// only to turn it back into a bsonx.Doc. We can fix this post beta1 when we refactor the driver
	// package to use bsoncore.Document instead of bsonx.Doc.
	if registry == nil {
		registry = bson.NewRegistryBuilder().Build()
	}
	switch tt := val.(type) {
	case nil:
		return nil, nil, ErrNilDocument
	case bsonx.Doc:
		val = tt.Copy()
	case []byte:
		// Slight optimization so we'll just use MarshalBSON and not go through the codec machinery.
		val = bson.Raw(tt)
	}

	// TODO(skriptble): Use a pool of these instead.
	buf := make([]byte, 0, 256)
	b, err := bson.MarshalAppendWithRegistry(registry, buf, val)
	if err != nil {
		return nil, nil, MarshalError{Value: val, Err: err}
	}

	d, err := bsonx.ReadDoc(b)
	if err != nil {
		return nil, nil, err
	}

	var id interface{}

	idx := d.IndexOf("_id")
	var idElem bsonx.Elem
	switch idx {
	case -1:
		idElem = bsonx.Elem{"_id", bsonx.ObjectID(primitive.NewObjectID())}
		d = append(d, bsonx.Elem{})
		copy(d[1:], d)
		d[0] = idElem
	default:
		idElem = d[idx]
		copy(d[1:idx+1], d[0:idx])
		d[0] = idElem
	}

	idBuf := make([]byte, 0, 256)
	t, data, err := idElem.Value.MarshalAppendBSONValue(idBuf[:0])
	if err != nil {
		return nil, nil, err
	}

	err = bson.RawValue{Type: t, Value: data}.UnmarshalWithRegistry(registry, &id)
	if err != nil {
		return nil, nil, err
	}

	return d, id, nil
}

func transformDocument(registry *bsoncodec.Registry, val interface{}) (bsonx.Doc, error) {
	if registry == nil {
		registry = bson.NewRegistryBuilder().Build()
	}
	if val == nil {
		return nil, ErrNilDocument
	}
	if doc, ok := val.(bsonx.Doc); ok {
		return doc.Copy(), nil
	}
	if bs, ok := val.([]byte); ok {
		// Slight optimization so we'll just use MarshalBSON and not go through the codec machinery.
		val = bson.Raw(bs)
	}

	// TODO(skriptble): Use a pool of these instead.
	buf := make([]byte, 0, 256)
	b, err := bson.MarshalAppendWithRegistry(registry, buf[:0], val)
	if err != nil {
		return nil, MarshalError{Value: val, Err: err}
	}
	return bsonx.ReadDoc(b)
}

func ensureID(d bsonx.Doc) (bsonx.Doc, interface{}) {
	var id interface{}

	elem, err := d.LookupElementErr("_id")
	switch err.(type) {
	case nil:
		id = elem
	default:
		oid := primitive.NewObjectID()
		d = append(d, bsonx.Elem{"_id", bsonx.ObjectID(oid)})
		id = oid
	}
	return d, id
}

func ensureDollarKey(doc bsonx.Doc) error {
	if len(doc) == 0 {
		return errors.New("update document must have at least one element")
	}
	if !strings.HasPrefix(doc[0].Key, "$") {
		return errors.New("update document must contain key beginning with '$'")
	}
	return nil
}

func transformAggregatePipeline(registry *bsoncodec.Registry, pipeline interface{}) (bsonx.Arr, error) {
	pipelineArr := bsonx.Arr{}
	switch t := pipeline.(type) {
	case bsoncodec.ValueMarshaler:
		btype, val, err := t.MarshalBSONValue()
		if err != nil {
			return nil, err
		}
		if btype != bsontype.Array {
			return nil, fmt.Errorf("ValueMarshaler returned a %v, but was expecting %v", btype, bsontype.Array)
		}
		err = pipelineArr.UnmarshalBSONValue(btype, val)
		if err != nil {
			return nil, err
		}
	default:
		val := reflect.ValueOf(t)
		if !val.IsValid() || (val.Kind() != reflect.Slice && val.Kind() != reflect.Array) {
			return nil, fmt.Errorf("can only transform slices and arrays into aggregation pipelines, but got %v", val.Kind())
		}
		for idx := 0; idx < val.Len(); idx++ {
			elem, err := transformDocument(registry, val.Index(idx).Interface())
			if err != nil {
				return nil, err
			}
			pipelineArr = append(pipelineArr, bsonx.Document(elem))
		}
	}

	return pipelineArr, nil
}

// Build the aggregation pipeline for the CountDocument command.
func countDocumentsAggregatePipeline(registry *bsoncodec.Registry, filter interface{}, opts *options.CountOptions) (bsonx.Arr, error) {
	pipeline := bsonx.Arr{}
	filterDoc, err := transformDocument(registry, filter)

	if err != nil {
		return nil, err
	}
	pipeline = append(pipeline, bsonx.Document(bsonx.Doc{{"$match", bsonx.Document(filterDoc)}}))

	if opts != nil {
		if opts.Skip != nil {
			pipeline = append(pipeline, bsonx.Document(bsonx.Doc{{"$skip", bsonx.Int64(*opts.Skip)}}))
		}
		if opts.Limit != nil {
			pipeline = append(pipeline, bsonx.Document(bsonx.Doc{{"$limit", bsonx.Int64(*opts.Limit)}}))
		}
	}

	pipeline = append(pipeline, bsonx.Document(bsonx.Doc{
		{"$group", bsonx.Document(bsonx.Doc{
			{"_id", bsonx.Int32(1)},
			{"n", bsonx.Document(bsonx.Doc{{"$sum", bsonx.Int32(1)}})},
		})},
	},
	))

	return pipeline, nil
}
