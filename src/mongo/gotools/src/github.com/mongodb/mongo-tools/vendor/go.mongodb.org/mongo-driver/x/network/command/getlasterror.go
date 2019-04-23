// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"

	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// GetLastError represents the getLastError command.
//
// The getLastError command is used for getting the last
// error from the last command on a connection.
//
// Since GetLastError only makes sense in the context of
// a single connection, there is no Dispatch method.
type GetLastError struct {
	Clock   *session.ClusterClock
	Session *session.Client

	err error
	res result.GetLastError
}

// Encode will encode this command into a wire message for the given server description.
func (gle *GetLastError) Encode() (wiremessage.WireMessage, error) {
	encoded, err := gle.encode()
	if err != nil {
		return nil, err
	}
	return encoded.Encode(description.SelectedServer{})
}

func (gle *GetLastError) encode() (*Read, error) {
	// This can probably just be a global variable that we reuse.
	cmd := bsonx.Doc{{"getLastError", bsonx.Int32(1)}}

	return &Read{
		Clock:    gle.Clock,
		DB:       "admin",
		ReadPref: readpref.Secondary(),
		Session:  gle.Session,
		Command:  cmd,
	}, nil
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (gle *GetLastError) Decode(wm wiremessage.WireMessage) *GetLastError {
	reply, ok := wm.(wiremessage.Reply)
	if !ok {
		gle.err = fmt.Errorf("unsupported response wiremessage type %T", wm)
		return gle
	}
	rdr, err := decodeCommandOpReply(reply)
	if err != nil {
		gle.err = err
		return gle
	}
	return gle.decode(rdr)
}

func (gle *GetLastError) decode(rdr bson.Raw) *GetLastError {
	err := bson.Unmarshal(rdr, &gle.res)
	if err != nil {
		gle.err = err
		return gle
	}

	return gle
}

// Result returns the result of a decoded wire message and server description.
func (gle *GetLastError) Result() (result.GetLastError, error) {
	if gle.err != nil {
		return result.GetLastError{}, gle.err
	}

	return gle.res, nil
}

// Err returns the error set on this command.
func (gle *GetLastError) Err() error { return gle.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (gle *GetLastError) RoundTrip(ctx context.Context, rw wiremessage.ReadWriter) (result.GetLastError, error) {
	cmd, err := gle.encode()
	if err != nil {
		return result.GetLastError{}, err
	}

	rdr, err := cmd.RoundTrip(ctx, description.SelectedServer{}, rw)
	if err != nil {
		return result.GetLastError{}, err
	}

	return gle.decode(rdr).Result()
}
