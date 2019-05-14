// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"
	"errors"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// ErrEmptyCursor is a signaling error when a cursor for list indexes is empty.
var ErrEmptyCursor = errors.New("empty cursor")

// ListIndexes represents the listIndexes command.
//
// The listIndexes command lists the indexes for a namespace.
type ListIndexes struct {
	Clock      *session.ClusterClock
	NS         Namespace
	CursorOpts []bsonx.Elem
	Opts       []bsonx.Elem
	Session    *session.Client

	result bson.Raw
	err    error
}

// Encode will encode this command into a wire message for the given server description.
func (li *ListIndexes) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	encoded, err := li.encode(desc)
	if err != nil {
		return nil, err
	}
	return encoded.Encode(desc)
}

func (li *ListIndexes) encode(desc description.SelectedServer) (*Read, error) {
	cmd := bsonx.Doc{{"listIndexes", bsonx.String(li.NS.Collection)}}
	cmd = append(cmd, li.Opts...)

	return &Read{
		Clock:   li.Clock,
		DB:      li.NS.DB,
		Command: cmd,
		Session: li.Session,
	}, nil
}

// Decode will decode the wire message using the provided server description. Errors during decoling
// are deferred until either the Result or Err methods are called.
func (li *ListIndexes) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *ListIndexes {
	rdr, err := (&Read{}).Decode(desc, wm).Result()
	if err != nil {
		if IsNotFound(err) {
			li.err = ErrEmptyCursor
			return li
		}
		li.err = err
		return li
	}

	return li.decode(desc, rdr)
}

func (li *ListIndexes) decode(desc description.SelectedServer, rdr bson.Raw) *ListIndexes {
	li.result = rdr
	return li
}

// Result returns the result of a decoded wire message and server description.
func (li *ListIndexes) Result() (bson.Raw, error) {
	if li.err != nil {
		return nil, li.err
	}
	return li.result, nil
}

// Err returns the error set on this command.
func (li *ListIndexes) Err() error { return li.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (li *ListIndexes) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (bson.Raw, error) {
	cmd, err := li.encode(desc)
	if err != nil {
		return nil, err
	}

	rdr, err := cmd.RoundTrip(ctx, desc, rw)
	if err != nil {
		if IsNotFound(err) {
			return nil, ErrEmptyCursor
		}
		return nil, err
	}

	return li.decode(desc, rdr).Result()
}
