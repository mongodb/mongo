// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// Aggregate represents the aggregate command.
//
// The aggregate command performs an aggregation.
type Aggregate struct {
	NS           Namespace
	Pipeline     bsonx.Arr
	CursorOpts   []bsonx.Elem
	Opts         []bsonx.Elem
	ReadPref     *readpref.ReadPref
	WriteConcern *writeconcern.WriteConcern
	ReadConcern  *readconcern.ReadConcern
	Clock        *session.ClusterClock
	Session      *session.Client

	result bson.Raw
	err    error
}

// Encode will encode this command into a wire message for the given server description.
func (a *Aggregate) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	cmd, err := a.encode(desc)
	if err != nil {
		return nil, err
	}

	return cmd.Encode(desc)
}

func (a *Aggregate) encode(desc description.SelectedServer) (*Read, error) {
	if err := a.NS.Validate(); err != nil {
		return nil, err
	}

	command := bsonx.Doc{
		{"aggregate", bsonx.String(a.NS.Collection)},
		{"pipeline", bsonx.Array(a.Pipeline)},
	}

	cursor := bsonx.Doc{}
	hasOutStage := a.HasDollarOut()

	for _, opt := range a.Opts {
		switch opt.Key {
		case "batchSize":
			if opt.Value.Int32() == 0 && hasOutStage {
				continue
			}
			cursor = append(cursor, opt)
		default:
			command = append(command, opt)
		}
	}
	command = append(command, bsonx.Elem{"cursor", bsonx.Document(cursor)})

	// add write concern because it won't be added by the Read command's Encode()
	if desc.WireVersion.Max >= 5 && hasOutStage && a.WriteConcern != nil {
		t, data, err := a.WriteConcern.MarshalBSONValue()
		if err != nil {
			return nil, err
		}
		var xval bsonx.Val
		err = xval.UnmarshalBSONValue(t, data)
		if err != nil {
			return nil, err
		}
		command = append(command, bsonx.Elem{Key: "writeConcern", Value: xval})
	}

	return &Read{
		DB:          a.NS.DB,
		Command:     command,
		ReadPref:    a.ReadPref,
		ReadConcern: a.ReadConcern,
		Clock:       a.Clock,
		Session:     a.Session,
	}, nil
}

// HasDollarOut returns true if the Pipeline field contains a $out stage.
func (a *Aggregate) HasDollarOut() bool {
	if a.Pipeline == nil {
		return false
	}
	if len(a.Pipeline) == 0 {
		return false
	}

	val := a.Pipeline[len(a.Pipeline)-1]

	doc, ok := val.DocumentOK()
	if !ok || len(doc) != 1 {
		return false
	}
	return doc[0].Key == "$out"
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (a *Aggregate) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *Aggregate {
	rdr, err := (&Read{}).Decode(desc, wm).Result()
	if err != nil {
		a.err = err
		return a
	}

	return a.decode(desc, rdr)
}

func (a *Aggregate) decode(desc description.SelectedServer, rdr bson.Raw) *Aggregate {
	a.result = rdr
	if val, err := rdr.LookupErr("writeConcernError"); err == nil {
		var wce result.WriteConcernError
		_ = val.Unmarshal(&wce)
		a.err = wce
	}
	return a
}

// Result returns the result of a decoded wire message and server description.
func (a *Aggregate) Result() (bson.Raw, error) {
	if a.err != nil {
		return nil, a.err
	}
	return a.result, nil
}

// Err returns the error set on this command.
func (a *Aggregate) Err() error { return a.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (a *Aggregate) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (bson.Raw, error) {
	cmd, err := a.encode(desc)
	if err != nil {
		return nil, err
	}

	rdr, err := cmd.RoundTrip(ctx, desc, rw)
	if err != nil {
		return nil, err
	}

	return a.decode(desc, rdr).Result()
}
