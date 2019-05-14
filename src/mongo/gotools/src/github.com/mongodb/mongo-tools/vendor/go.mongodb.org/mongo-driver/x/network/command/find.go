// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// Find represents the find command.
//
// The find command finds documents within a collection that match a filter.
type Find struct {
	NS          Namespace
	Filter      bsonx.Doc
	CursorOpts  []bsonx.Elem
	Opts        []bsonx.Elem
	ReadPref    *readpref.ReadPref
	ReadConcern *readconcern.ReadConcern
	Clock       *session.ClusterClock
	Session     *session.Client

	result bson.Raw
	err    error
}

// Encode will encode this command into a wire message for the given server description.
func (f *Find) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	cmd, err := f.encode(desc)
	if err != nil {
		return nil, err
	}

	return cmd.Encode(desc)
}

func (f *Find) encode(desc description.SelectedServer) (*Read, error) {
	if err := f.NS.Validate(); err != nil {
		return nil, err
	}

	command := bsonx.Doc{{"find", bsonx.String(f.NS.Collection)}}

	if f.Filter != nil {
		command = append(command, bsonx.Elem{"filter", bsonx.Document(f.Filter)})
	}

	command = append(command, f.Opts...)

	return &Read{
		Clock:       f.Clock,
		DB:          f.NS.DB,
		ReadPref:    f.ReadPref,
		Command:     command,
		ReadConcern: f.ReadConcern,
		Session:     f.Session,
	}, nil
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (f *Find) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *Find {
	rdr, err := (&Read{}).Decode(desc, wm).Result()
	if err != nil {
		f.err = err
		return f
	}

	return f.decode(desc, rdr)
}

func (f *Find) decode(desc description.SelectedServer, rdr bson.Raw) *Find {
	f.result = rdr
	return f
}

// Result returns the result of a decoded wire message and server description.
func (f *Find) Result() (bson.Raw, error) {
	if f.err != nil {
		return nil, f.err
	}

	return f.result, nil
}

// Err returns the error set on this command.
func (f *Find) Err() error { return f.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (f *Find) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (bson.Raw, error) {
	cmd, err := f.encode(desc)
	if err != nil {
		return nil, err
	}

	rdr, err := cmd.RoundTrip(ctx, desc, rw)
	if err != nil {
		return nil, err
	}

	return f.decode(desc, rdr).Result()
}
