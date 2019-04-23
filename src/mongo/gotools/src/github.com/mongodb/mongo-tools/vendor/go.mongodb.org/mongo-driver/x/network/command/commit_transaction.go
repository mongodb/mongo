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

// CommitTransaction represents the commitTransaction() command
type CommitTransaction struct {
	Session *session.Client
	err     error
	result  result.TransactionResult
}

// Encode will encode this command into a wiremessage for the given server description.
func (ct *CommitTransaction) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	cmd := ct.encode(desc)
	return cmd.Encode(desc)
}

func (ct *CommitTransaction) encode(desc description.SelectedServer) *Write {
	cmd := bsonx.Doc{{"commitTransaction", bsonx.Int32(1)}}
	return &Write{
		DB:           "admin",
		Command:      cmd,
		Session:      ct.Session,
		WriteConcern: ct.Session.CurrentWc,
	}
}

// Decode will decode the wire message using the provided server description. Errors during decoding are deferred until
// either the Result or Err methods are called.
func (ct *CommitTransaction) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *CommitTransaction {
	rdr, err := (&Write{}).Decode(desc, wm).Result()
	if err != nil {
		ct.err = err
		return ct
	}

	return ct.decode(desc, rdr)
}

func (ct *CommitTransaction) decode(desc description.SelectedServer, rdr bson.Raw) *CommitTransaction {
	ct.err = bson.Unmarshal(rdr, &ct.result)
	if ct.err == nil && ct.result.WriteConcernError != nil {
		ct.err = Error{
			Code:    int32(ct.result.WriteConcernError.Code),
			Message: ct.result.WriteConcernError.ErrMsg,
		}
	}
	return ct
}

// Result returns the result of a decoded wire message and server description.
func (ct *CommitTransaction) Result() (result.TransactionResult, error) {
	if ct.err != nil {
		return result.TransactionResult{}, ct.err
	}

	return ct.result, nil
}

// Err returns the error set on this command
func (ct *CommitTransaction) Err() error {
	return ct.err
}

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter
func (ct *CommitTransaction) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (result.TransactionResult, error) {
	cmd := ct.encode(desc)
	rdr, err := cmd.RoundTrip(ctx, desc, rw)
	if err != nil {
		return result.TransactionResult{}, err
	}

	return ct.decode(desc, rdr).Result()
}
