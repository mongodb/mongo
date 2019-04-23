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

// DropDatabase represents the DropDatabase command.
//
// The DropDatabases command drops database.
type DropDatabase struct {
	DB           string
	WriteConcern *writeconcern.WriteConcern
	Clock        *session.ClusterClock
	Session      *session.Client

	result bson.Raw
	err    error
}

// Encode will encode this command into a wire message for the given server description.
func (dd *DropDatabase) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	cmd, err := dd.encode(desc)
	if err != nil {
		return nil, err
	}

	return cmd.Encode(desc)
}

func (dd *DropDatabase) encode(desc description.SelectedServer) (*Write, error) {
	cmd := bsonx.Doc{{"dropDatabase", bsonx.Int32(1)}}

	write := &Write{
		Clock:   dd.Clock,
		DB:      dd.DB,
		Command: cmd,
		Session: dd.Session,
	}
	if desc.WireVersion != nil && desc.WireVersion.Max >= 5 {
		write.WriteConcern = dd.WriteConcern
	}
	return write, nil
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (dd *DropDatabase) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *DropDatabase {
	rdr, err := (&Write{}).Decode(desc, wm).Result()
	if err != nil {
		dd.err = err
		return dd
	}

	return dd.decode(desc, rdr)
}

func (dd *DropDatabase) decode(desc description.SelectedServer, rdr bson.Raw) *DropDatabase {
	dd.result = rdr
	return dd
}

// Result returns the result of a decoded wire message and server description.
func (dd *DropDatabase) Result() (bson.Raw, error) {
	if dd.err != nil {
		return nil, dd.err
	}

	return dd.result, nil
}

// Err returns the error set on this command.
func (dd *DropDatabase) Err() error { return dd.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (dd *DropDatabase) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (bson.Raw, error) {
	cmd, err := dd.encode(desc)
	if err != nil {
		return nil, err
	}

	rdr, err := cmd.RoundTrip(ctx, desc, rw)
	if err != nil {
		return nil, err
	}

	return dd.decode(desc, rdr).Result()
}
