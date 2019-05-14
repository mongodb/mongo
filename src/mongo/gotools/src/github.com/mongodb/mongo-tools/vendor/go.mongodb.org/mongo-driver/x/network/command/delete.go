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

// Delete represents the delete command.
//
// The delete command executes a delete with a given set of delete documents
// and options.
type Delete struct {
	ContinueOnError bool
	NS              Namespace
	Deletes         []bsonx.Doc
	Opts            []bsonx.Elem
	WriteConcern    *writeconcern.WriteConcern
	Clock           *session.ClusterClock
	Session         *session.Client

	batches []*WriteBatch
	result  result.Delete
	err     error
}

// Encode will encode this command into a wire message for the given server description.
func (d *Delete) Encode(desc description.SelectedServer) ([]wiremessage.WireMessage, error) {
	err := d.encode(desc)
	if err != nil {
		return nil, err
	}

	return batchesToWireMessage(d.batches, desc)
}

func (d *Delete) encode(desc description.SelectedServer) error {
	batches, err := splitBatches(d.Deletes, int(desc.MaxBatchCount), int(desc.MaxDocumentSize))
	if err != nil {
		return err
	}

	for _, docs := range batches {
		cmd, err := d.encodeBatch(docs, desc)
		if err != nil {
			return err
		}

		d.batches = append(d.batches, cmd)
	}

	return nil
}

func (d *Delete) encodeBatch(docs []bsonx.Doc, desc description.SelectedServer) (*WriteBatch, error) {
	copyDocs := make([]bsonx.Doc, 0, len(docs))
	for _, doc := range docs {
		copyDocs = append(copyDocs, doc.Copy())
	}

	var options []bsonx.Elem
	for _, opt := range d.Opts {
		if opt.Key == "collation" {
			for idx := range copyDocs {
				copyDocs[idx] = append(copyDocs[idx], opt)
			}
		} else {
			options = append(options, opt)
		}
	}

	command, err := encodeBatch(copyDocs, options, DeleteCommand, d.NS.Collection)
	if err != nil {
		return nil, err
	}

	return &WriteBatch{
		&Write{
			Clock:        d.Clock,
			DB:           d.NS.DB,
			Command:      command,
			WriteConcern: d.WriteConcern,
			Session:      d.Session,
		},
		len(docs),
	}, nil
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (d *Delete) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *Delete {
	rdr, err := (&Write{}).Decode(desc, wm).Result()
	if err != nil {
		d.err = err
		return d
	}

	return d.decode(desc, rdr)
}

func (d *Delete) decode(desc description.SelectedServer, rdr bson.Raw) *Delete {
	d.err = bson.Unmarshal(rdr, &d.result)
	return d
}

// Result returns the result of a decoded wire message and server description.
func (d *Delete) Result() (result.Delete, error) {
	if d.err != nil {
		return result.Delete{}, d.err
	}
	return d.result, nil
}

// Err returns the error set on this command.
func (d *Delete) Err() error { return d.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (d *Delete) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (result.Delete, error) {
	if d.batches == nil {
		if err := d.encode(desc); err != nil {
			return result.Delete{}, err
		}
	}

	r, batches, err := roundTripBatches(
		ctx, desc, rw,
		d.batches,
		d.ContinueOnError,
		d.Session,
		DeleteCommand,
	)

	if batches != nil {
		d.batches = batches
	}

	if err != nil {
		return result.Delete{}, err
	}

	return r.(result.Delete), nil
}
