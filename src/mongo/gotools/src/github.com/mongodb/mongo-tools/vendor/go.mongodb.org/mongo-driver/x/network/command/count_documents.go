// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"
	"errors"

	"go.mongodb.org/mongo-driver/bson/bsontype"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// CountDocuments represents the CountDocuments command.
//
// The countDocuments command counts how many documents in a collection match the given query.
type CountDocuments struct {
	NS          Namespace
	Pipeline    bsonx.Arr
	Opts        []bsonx.Elem
	ReadPref    *readpref.ReadPref
	ReadConcern *readconcern.ReadConcern
	Clock       *session.ClusterClock
	Session     *session.Client

	result int64
	err    error
}

// Encode will encode this command into a wire message for the given server description.
func (c *CountDocuments) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	if err := c.NS.Validate(); err != nil {
		return nil, err
	}
	command := bsonx.Doc{{"aggregate", bsonx.String(c.NS.Collection)}, {"pipeline", bsonx.Array(c.Pipeline)}}

	command = append(command, bsonx.Elem{"cursor", bsonx.Document(bsonx.Doc{})})
	command = append(command, c.Opts...)

	return (&Read{DB: c.NS.DB, ReadPref: c.ReadPref, Command: command, Session: c.Session}).Encode(desc)
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (c *CountDocuments) Decode(ctx context.Context, desc description.SelectedServer, wm wiremessage.WireMessage) *CountDocuments {
	rdr, err := (&Read{Session: c.Session}).Decode(desc, wm).Result()
	if err != nil {
		c.err = err
		return c
	}

	cursor, err := rdr.LookupErr("cursor")
	if err != nil || cursor.Type != bsontype.EmbeddedDocument {
		c.err = errors.New("Invalid response from server, no 'cursor' field")
		return c
	}
	batch, err := cursor.Document().LookupErr("firstBatch")
	if err != nil || batch.Type != bsontype.Array {
		c.err = errors.New("Invalid response from server, no 'firstBatch' field")
		return c
	}

	elem, err := batch.Array().IndexErr(0)
	if err != nil || elem.Value().Type != bsontype.EmbeddedDocument {
		c.result = 0
		return c
	}

	val, err := elem.Value().Document().LookupErr("n")
	if err != nil {
		c.err = errors.New("Invalid response from server, no 'n' field")
		return c
	}

	switch val.Type {
	case bsontype.Int32:
		c.result = int64(val.Int32())
	case bsontype.Int64:
		c.result = val.Int64()
	default:
		c.err = errors.New("Invalid response from server, value field is not a number")
	}

	return c
}

// Result returns the result of a decoded wire message and server description.
func (c *CountDocuments) Result() (int64, error) {
	if c.err != nil {
		return 0, c.err
	}
	return c.result, nil
}

// Err returns the error set on this command.
func (c *CountDocuments) Err() error { return c.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (c *CountDocuments) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (int64, error) {
	wm, err := c.Encode(desc)
	if err != nil {
		return 0, err
	}

	err = rw.WriteWireMessage(ctx, wm)
	if err != nil {
		if _, ok := err.(Error); ok {
			return 0, err
		}
		// Connection errors are transient
		c.Session.ClearPinnedServer()
		return 0, Error{Message: err.Error(), Labels: []string{TransientTransactionError, NetworkError}}
	}

	wm, err = rw.ReadWireMessage(ctx)
	if err != nil {
		if _, ok := err.(Error); ok {
			return 0, err
		}
		// Connection errors are transient
		c.Session.ClearPinnedServer()
		return 0, Error{Message: err.Error(), Labels: []string{TransientTransactionError, NetworkError}}
	}

	return c.Decode(ctx, desc, wm).Result()
}
