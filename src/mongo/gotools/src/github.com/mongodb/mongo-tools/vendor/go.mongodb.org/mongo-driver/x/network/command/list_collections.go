// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// ListCollections represents the listCollections command.
//
// The listCollections command lists the collections in a database.
type ListCollections struct {
	Clock      *session.ClusterClock
	DB         string
	Filter     bsonx.Doc
	CursorOpts []bsonx.Elem
	Opts       []bsonx.Elem
	ReadPref   *readpref.ReadPref
	Session    *session.Client

	result bson.Raw
	err    error
}

// Encode will encode this command into a wire message for the given server description.
func (lc *ListCollections) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	encoded, err := lc.encode(desc)
	if err != nil {
		return nil, err
	}
	return encoded.Encode(desc)
}

func (lc *ListCollections) encode(desc description.SelectedServer) (*Read, error) {
	cmd := bsonx.Doc{{"listCollections", bsonx.Int32(1)}}

	if lc.Filter != nil {
		cmd = append(cmd, bsonx.Elem{"filter", bsonx.Document(lc.Filter)})
	}
	cmd = append(cmd, lc.Opts...)

	return &Read{
		Clock:    lc.Clock,
		DB:       lc.DB,
		Command:  cmd,
		ReadPref: lc.ReadPref,
		Session:  lc.Session,
	}, nil
}

// Decode will decode the wire message using the provided server description. Errors during decolcng
// are deferred until either the Result or Err methods are called.
func (lc *ListCollections) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *ListCollections {
	rdr, err := (&Read{}).Decode(desc, wm).Result()
	if err != nil {
		lc.err = err
		return lc
	}
	return lc.decode(desc, rdr)
}

func (lc *ListCollections) decode(desc description.SelectedServer, rdr bson.Raw) *ListCollections {
	lc.result = rdr
	return lc
}

// Result returns the result of a decoded wire message and server description.
func (lc *ListCollections) Result() (bson.Raw, error) {
	if lc.err != nil {
		return nil, lc.err
	}
	return lc.result, nil
}

// Err returns the error set on this command.
func (lc *ListCollections) Err() error { return lc.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (lc *ListCollections) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (bson.Raw, error) {
	cmd, err := lc.encode(desc)
	if err != nil {
		return nil, err
	}

	rdr, err := cmd.RoundTrip(ctx, desc, rw)
	if err != nil {
		return nil, err
	}

	return lc.decode(desc, rdr).Result()
}
