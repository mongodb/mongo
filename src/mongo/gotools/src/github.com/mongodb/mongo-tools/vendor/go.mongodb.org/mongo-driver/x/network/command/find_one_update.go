// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// FindOneAndUpdate represents the findOneAndUpdate operation.
//
// The findOneAndUpdate command modifies and returns a single document.
type FindOneAndUpdate struct {
	NS           Namespace
	Query        bsonx.Doc
	Update       bsonx.Doc
	Opts         []bsonx.Elem
	WriteConcern *writeconcern.WriteConcern
	Clock        *session.ClusterClock
	Session      *session.Client

	result result.FindAndModify
	err    error
}

// Encode will encode this command into a wire message for the given server description.
func (f *FindOneAndUpdate) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	cmd, err := f.encode(desc)
	if err != nil {
		return nil, err
	}

	return cmd.Encode(desc)
}

func (f *FindOneAndUpdate) encode(desc description.SelectedServer) (*Write, error) {
	if err := f.NS.Validate(); err != nil {
		return nil, err
	}

	command := bsonx.Doc{
		{"findAndModify", bsonx.String(f.NS.Collection)},
		{"query", bsonx.Document(f.Query)},
		{"update", bsonx.Document(f.Update)},
	}
	command = append(command, f.Opts...)

	write := &Write{
		Clock:   f.Clock,
		DB:      f.NS.DB,
		Command: command,
		Session: f.Session,
	}
	if desc.WireVersion != nil && desc.WireVersion.Max >= 4 {
		write.WriteConcern = f.WriteConcern
	}
	return write, nil
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (f *FindOneAndUpdate) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *FindOneAndUpdate {
	rdr, err := (&Write{}).Decode(desc, wm).Result()
	if err != nil {
		f.err = err
		return f
	}

	return f.decode(desc, rdr)
}

func (f *FindOneAndUpdate) decode(desc description.SelectedServer, rdr bson.Raw) *FindOneAndUpdate {
	f.result, f.err = unmarshalFindAndModifyResult(rdr)
	return f
}

// Result returns the result of a decoded wire message and server description.
func (f *FindOneAndUpdate) Result() (result.FindAndModify, error) {
	if f.err != nil {
		return result.FindAndModify{}, f.err
	}
	return f.result, nil
}

// Err returns the error set on this command.
func (f *FindOneAndUpdate) Err() error { return f.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (f *FindOneAndUpdate) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (result.FindAndModify, error) {
	cmd, err := f.encode(desc)
	if err != nil {
		return result.FindAndModify{}, err
	}

	rdr, err := cmd.RoundTrip(ctx, desc, rw)
	if err != nil {
		return result.FindAndModify{}, err
	}

	return f.decode(desc, rdr).Result()
}
