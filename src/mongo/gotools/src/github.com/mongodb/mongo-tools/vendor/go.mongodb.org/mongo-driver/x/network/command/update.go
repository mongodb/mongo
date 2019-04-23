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
	"go.mongodb.org/mongo-driver/x/network/result"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// Update represents the update command.
//
// The update command updates a set of documents with the database.
type Update struct {
	ContinueOnError bool
	Clock           *session.ClusterClock
	NS              Namespace
	Docs            []bsonx.Doc
	Opts            []bsonx.Elem
	WriteConcern    *writeconcern.WriteConcern
	Session         *session.Client

	batches []*WriteBatch
	result  result.Update
	err     error
}

// Encode will encode this command into a wire message for the given server description.
func (u *Update) Encode(desc description.SelectedServer) ([]wiremessage.WireMessage, error) {
	err := u.encode(desc)
	if err != nil {
		return nil, err
	}

	return batchesToWireMessage(u.batches, desc)
}

func (u *Update) encode(desc description.SelectedServer) error {
	batches, err := splitBatches(u.Docs, int(desc.MaxBatchCount), int(desc.MaxDocumentSize))
	if err != nil {
		return err
	}

	for _, docs := range batches {
		cmd, err := u.encodeBatch(docs, desc)
		if err != nil {
			return err
		}

		u.batches = append(u.batches, cmd)
	}

	return nil
}

func (u *Update) encodeBatch(docs []bsonx.Doc, desc description.SelectedServer) (*WriteBatch, error) {
	copyDocs := make([]bsonx.Doc, 0, len(docs)) // copy of all the documents
	for _, doc := range docs {
		newDoc := doc.Copy()
		copyDocs = append(copyDocs, newDoc)
	}

	var options []bsonx.Elem
	for _, opt := range u.Opts {
		switch opt.Key {
		case "upsert", "collation", "arrayFilters":
			// options that are encoded on each individual document
			for idx := range copyDocs {
				copyDocs[idx] = append(copyDocs[idx], opt)
			}
		default:
			options = append(options, opt)
		}
	}

	command, err := encodeBatch(copyDocs, options, UpdateCommand, u.NS.Collection)
	if err != nil {
		return nil, err
	}

	return &WriteBatch{
		&Write{
			Clock:        u.Clock,
			DB:           u.NS.DB,
			Command:      command,
			WriteConcern: u.WriteConcern,
			Session:      u.Session,
		},
		len(docs),
	}, nil
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (u *Update) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *Update {
	rdr, err := (&Write{}).Decode(desc, wm).Result()
	if err != nil {
		u.err = err
		return u
	}
	return u.decode(desc, rdr)
}

func (u *Update) decode(desc description.SelectedServer, rdr bson.Raw) *Update {
	u.err = bson.Unmarshal(rdr, &u.result)
	return u
}

// Result returns the result of a decoded wire message and server description.
func (u *Update) Result() (result.Update, error) {
	if u.err != nil {
		return result.Update{}, u.err
	}
	return u.result, nil
}

// Err returns the error set on this command.
func (u *Update) Err() error { return u.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (u *Update) RoundTrip(
	ctx context.Context,
	desc description.SelectedServer,
	rw wiremessage.ReadWriter,
) (result.Update, error) {
	if u.batches == nil {
		err := u.encode(desc)
		if err != nil {
			return result.Update{}, err
		}
	}

	r, batches, err := roundTripBatches(
		ctx, desc, rw,
		u.batches,
		u.ContinueOnError,
		u.Session,
		UpdateCommand,
	)

	// if there are leftover batches, save them for retry
	if batches != nil {
		u.batches = batches
	}

	if err != nil {
		return result.Update{}, err
	}

	return r.(result.Update), nil
}
