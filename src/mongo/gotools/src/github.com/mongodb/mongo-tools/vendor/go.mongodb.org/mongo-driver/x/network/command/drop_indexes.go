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
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// DropIndexes represents the dropIndexes command.
//
// The dropIndexes command drops indexes for a namespace.
type DropIndexes struct {
	NS           Namespace
	Index        string
	Opts         []bsonx.Elem
	WriteConcern *writeconcern.WriteConcern
	Clock        *session.ClusterClock
	Session      *session.Client

	result bson.Raw
	err    error
}

// Encode will encode this command into a wire message for the given server description.
func (di *DropIndexes) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	cmd, err := di.encode(desc)
	if err != nil {
		return nil, err
	}

	return cmd.Encode(desc)
}

func (di *DropIndexes) encode(desc description.SelectedServer) (*Write, error) {
	cmd := bsonx.Doc{
		{"dropIndexes", bsonx.String(di.NS.Collection)},
		{"index", bsonx.String(di.Index)},
	}
	cmd = append(cmd, di.Opts...)

	write := &Write{
		Clock:   di.Clock,
		DB:      di.NS.DB,
		Command: cmd,
		Session: di.Session,
	}
	if desc.WireVersion != nil && desc.WireVersion.Max >= 5 {
		write.WriteConcern = di.WriteConcern
	}
	return write, nil
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (di *DropIndexes) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *DropIndexes {
	rdr, err := (&Write{}).Decode(desc, wm).Result()
	if err != nil {
		di.err = err
		return di
	}

	return di.decode(desc, rdr)
}

func (di *DropIndexes) decode(desc description.SelectedServer, rdr bson.Raw) *DropIndexes {
	di.result = rdr
	return di
}

// Result returns the result of a decoded wire message and server description.
func (di *DropIndexes) Result() (bson.Raw, error) {
	if di.err != nil {
		return nil, di.err
	}

	return di.result, nil
}

// Err returns the error set on this command.
func (di *DropIndexes) Err() error { return di.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (di *DropIndexes) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (bson.Raw, error) {
	cmd, err := di.encode(desc)
	if err != nil {
		return nil, err
	}

	di.result, err = cmd.RoundTrip(ctx, desc, rw)
	if err != nil {
		return nil, err
	}

	return di.Result()
}
