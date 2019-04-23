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

// AbortTransaction represents the abortTransaction() command
type AbortTransaction struct {
	Session *session.Client
	err     error
	result  result.TransactionResult
}

// Encode will encode this command into a wiremessage for the given server description.
func (at *AbortTransaction) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	cmd := at.encode(desc)
	return cmd.Encode(desc)
}

func (at *AbortTransaction) encode(desc description.SelectedServer) *Write {
	cmd := bsonx.Doc{{"abortTransaction", bsonx.Int32(1)}}
	return &Write{
		DB:           "admin",
		Command:      cmd,
		Session:      at.Session,
		WriteConcern: at.Session.CurrentWc,
	}
}

// Decode will decode the wire message using the provided server description. Errors during decoding are deferred until
// either the Result or Err methods are called.
func (at *AbortTransaction) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *AbortTransaction {
	rdr, err := (&Write{}).Decode(desc, wm).Result()
	if err != nil {
		at.err = err
		return at
	}

	return at.decode(desc, rdr)
}

func (at *AbortTransaction) decode(desc description.SelectedServer, rdr bson.Raw) *AbortTransaction {
	at.err = bson.Unmarshal(rdr, &at.result)
	if at.err == nil && at.result.WriteConcernError != nil {
		at.err = Error{
			Code:    int32(at.result.WriteConcernError.Code),
			Message: at.result.WriteConcernError.ErrMsg,
		}
	}
	return at
}

// Result returns the result of a decoded wire message and server description.
func (at *AbortTransaction) Result() (result.TransactionResult, error) {
	if at.err != nil {
		return result.TransactionResult{}, at.err
	}

	return at.result, nil
}

// Err returns the error set on this command
func (at *AbortTransaction) Err() error {
	return at.err
}

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter
func (at *AbortTransaction) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (result.TransactionResult, error) {
	cmd := at.encode(desc)
	rdr, err := cmd.RoundTrip(ctx, desc, rw)
	if err != nil {
		return result.TransactionResult{}, err
	}

	return at.decode(desc, rdr).Result()
}
