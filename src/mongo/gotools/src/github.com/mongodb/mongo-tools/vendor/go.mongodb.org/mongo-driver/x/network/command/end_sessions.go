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
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// must be sent to admin db
// { endSessions: [ {id: uuid}, ... ], $clusterTime: ... }
// only send $clusterTime when gossiping the cluster time
// send 10k sessions at a time

// EndSessions represents an endSessions command.
type EndSessions struct {
	Clock      *session.ClusterClock
	SessionIDs []bsonx.Doc

	results []result.EndSessions
	errors  []error
}

// BatchSize is the max number of sessions to be included in 1 endSessions command.
const BatchSize = 10000

func (es *EndSessions) split() [][]bsonx.Doc {
	batches := [][]bsonx.Doc{}
	docIndex := 0
	totalNumDocs := len(es.SessionIDs)

createBatches:
	for {
		batch := []bsonx.Doc{}

		for i := 0; i < BatchSize; i++ {
			if docIndex == totalNumDocs {
				break createBatches
			}

			batch = append(batch, es.SessionIDs[docIndex])
			docIndex++
		}

		batches = append(batches, batch)
	}

	return batches
}

func (es *EndSessions) encodeBatch(batch []bsonx.Doc, desc description.SelectedServer) *Write {
	vals := make(bsonx.Arr, 0, len(batch))
	for _, doc := range batch {
		vals = append(vals, bsonx.Document(doc))
	}

	cmd := bsonx.Doc{{"endSessions", bsonx.Array(vals)}}

	return &Write{
		Clock:   es.Clock,
		DB:      "admin",
		Command: cmd,
	}
}

// Encode will encode this command into a series of wire messages for the given server description.
func (es *EndSessions) Encode(desc description.SelectedServer) ([]wiremessage.WireMessage, error) {
	cmds := es.encode(desc)
	wms := make([]wiremessage.WireMessage, len(cmds))

	for _, cmd := range cmds {
		wm, err := cmd.Encode(desc)
		if err != nil {
			return nil, err
		}

		wms = append(wms, wm)
	}

	return wms, nil
}

func (es *EndSessions) encode(desc description.SelectedServer) []*Write {
	out := []*Write{}
	batches := es.split()

	for _, batch := range batches {
		out = append(out, es.encodeBatch(batch, desc))
	}

	return out
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (es *EndSessions) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *EndSessions {
	rdr, err := (&Write{}).Decode(desc, wm).Result()
	if err != nil {
		es.errors = append(es.errors, err)
		return es
	}

	return es.decode(desc, rdr)
}

func (es *EndSessions) decode(desc description.SelectedServer, rdr bson.Raw) *EndSessions {
	var res result.EndSessions
	es.errors = append(es.errors, bson.Unmarshal(rdr, &res))
	es.results = append(es.results, res)
	return es
}

// Result returns the results of the decoded wire messages.
func (es *EndSessions) Result() ([]result.EndSessions, []error) {
	return es.results, es.errors
}

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter
func (es *EndSessions) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) ([]result.EndSessions, []error) {
	cmds := es.encode(desc)

	for _, cmd := range cmds {
		rdr, _ := cmd.RoundTrip(ctx, desc, rw) // ignore any errors returned by the command
		es.decode(desc, rdr)
	}

	return es.Result()
}
