// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// StartSession represents a startSession command
type StartSession struct {
	Clock  *session.ClusterClock
	result result.StartSession
	err    error
}

// Encode will encode this command into a wiremessage for the given server description.
func (ss *StartSession) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	cmd := ss.encode(desc)
	return cmd.Encode(desc)
}

func (ss *StartSession) encode(desc description.SelectedServer) *Write {
	cmd := bsonx.Doc{{"startSession", bsonx.Int32(1)}}
	return &Write{
		Clock:   ss.Clock,
		DB:      "admin",
		Command: cmd,
	}
}

// Decode will decode the wire message using the provided server description. Errors during decoding are deferred until
// either the Result or Err methods are called.
func (ss *StartSession) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *StartSession {
	rdr, err := (&Write{}).Decode(desc, wm).Result()
	if err != nil {
		ss.err = err
		return ss
	}

	return ss.decode(desc, rdr)
}

func (ss *StartSession) decode(desc description.SelectedServer, rdr bson.Raw) *StartSession {
	ss.err = bson.Unmarshal(rdr, &ss.result)
	return ss
}

// Result returns the result of a decoded wire message and server description.
func (ss *StartSession) Result() (result.StartSession, error) {
	if ss.err != nil {
		return result.StartSession{}, ss.err
	}

	return ss.result, nil
}

// Err returns the error set on this command
func (ss *StartSession) Err() error {
	return ss.err
}

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter
func (ss *StartSession) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (result.StartSession, error) {
	cmd := ss.encode(desc)
	rdr, err := cmd.RoundTrip(ctx, desc, rw)
	if err != nil {
		return result.StartSession{}, err
	}

	return ss.decode(desc, rdr).Result()
}
