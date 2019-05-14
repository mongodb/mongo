// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package driverlegacy

import (
	"context"
	"fmt"
	"time"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
)

// Aggregate handles the full cycle dispatch and execution of an aggregate command against the provided
// topology.
func Aggregate(
	ctx context.Context,
	cmd command.Aggregate,
	topo *topology.Topology,
	readSelector, writeSelector description.ServerSelector,
	clientID uuid.UUID,
	pool *session.Pool,
	registry *bsoncodec.Registry,
	opts ...*options.AggregateOptions,
) (*BatchCursor, error) {

	dollarOut := cmd.HasDollarOut()

	var ss *topology.SelectedServer
	var err error
	if cmd.Session != nil && cmd.Session.PinnedServer != nil {
		writeSelector = cmd.Session.PinnedServer
		readSelector = cmd.Session.PinnedServer
	}
	switch dollarOut {
	case true:
		ss, err = topo.SelectServer(ctx, writeSelector)
		if err != nil {
			return nil, err
		}
	case false:
		ss, err = topo.SelectServer(ctx, readSelector)
		if err != nil {
			return nil, err
		}
	}

	desc := ss.Description()
	conn, err := ss.Connection(ctx)
	if err != nil {
		return nil, err
	}

	defer conn.Close()

	rp, err := getReadPrefBasedOnTransaction(cmd.ReadPref, cmd.Session)
	if err != nil {
		return nil, err
	}
	cmd.ReadPref = rp

	// If no explicit session and deployment supports sessions, start implicit session.
	if cmd.Session == nil && topo.SupportsSessions() {
		cmd.Session, err = session.NewClientSession(pool, clientID, session.Implicit)
		if err != nil {
			return nil, err
		}
	}

	aggOpts := options.MergeAggregateOptions(opts...)

	if aggOpts.AllowDiskUse != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"allowDiskUse", bsonx.Boolean(*aggOpts.AllowDiskUse)})
	}
	var batchSize int32
	if aggOpts.BatchSize != nil {
		elem := bsonx.Elem{"batchSize", bsonx.Int32(*aggOpts.BatchSize)}
		cmd.Opts = append(cmd.Opts, elem)
		cmd.CursorOpts = append(cmd.CursorOpts, elem)
		batchSize = *aggOpts.BatchSize
	}
	if aggOpts.BypassDocumentValidation != nil && desc.WireVersion.Includes(4) {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"bypassDocumentValidation", bsonx.Boolean(*aggOpts.BypassDocumentValidation)})
	}
	if aggOpts.Collation != nil {
		if desc.WireVersion.Max < 5 {
			return nil, ErrCollation
		}
		collDoc, err := bsonx.ReadDoc(aggOpts.Collation.ToDocument())
		if err != nil {
			return nil, err
		}
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"collation", bsonx.Document(collDoc)})
	}
	if aggOpts.MaxTime != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"maxTimeMS", bsonx.Int64(int64(*aggOpts.MaxTime / time.Millisecond))})
	}
	if aggOpts.MaxAwaitTime != nil {
		// specified as maxTimeMS on getMore commands
		cmd.CursorOpts = append(cmd.CursorOpts, bsonx.Elem{
			"maxTimeMS", bsonx.Int64(int64(*aggOpts.MaxAwaitTime / time.Millisecond)),
		})
	}
	if aggOpts.Comment != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"comment", bsonx.String(*aggOpts.Comment)})
	}
	if aggOpts.Hint != nil {
		hintElem, err := interfaceToElement("hint", aggOpts.Hint, registry)
		if err != nil {
			return nil, err
		}

		cmd.Opts = append(cmd.Opts, hintElem)
	}

	res, err := cmd.RoundTrip(ctx, desc, conn)
	if err != nil {
		if wce, ok := err.(result.WriteConcernError); ok {
			ss.ProcessWriteConcernError(&wce)
		}
		closeImplicitSession(cmd.Session)
		return nil, err
	}

	if desc.WireVersion.Max < 4 {
		return buildLegacyCommandBatchCursor(res, batchSize, ss.Server)
	}

	return NewBatchCursor(bsoncore.Document(res), cmd.Session, cmd.Clock, ss.Server, cmd.CursorOpts...)
}

func buildLegacyCommandBatchCursor(rdr bson.Raw, batchSize int32, server *topology.Server) (*BatchCursor, error) {
	firstBatchDocs, ns, cursorID, err := getCursorValues(rdr)
	if err != nil {
		return nil, err
	}

	return NewLegacyBatchCursor(ns, cursorID, firstBatchDocs, 0, batchSize, server)
}

// get the firstBatch, cursor ID, and namespace from a bson.Raw
//
// TODO(GODRIVER-617): Change the documents return value into []bsoncore.Document.
func getCursorValues(result bson.Raw) (*bsoncore.DocumentSequence, command.Namespace, int64, error) {
	cur, err := result.LookupErr("cursor")
	if err != nil {
		return nil, command.Namespace{}, 0, err
	}
	if cur.Type != bson.TypeEmbeddedDocument {
		return nil, command.Namespace{}, 0, fmt.Errorf("cursor should be an embedded document but it is a BSON %s", cur.Type)
	}

	elems, err := cur.Document().Elements()
	if err != nil {
		return nil, command.Namespace{}, 0, err
	}

	var ok bool
	var namespace command.Namespace
	var cursorID int64
	batch := new(bsoncore.DocumentSequence)

	for _, elem := range elems {
		switch elem.Key() {
		case "firstBatch":
			arr, ok := elem.Value().ArrayOK()
			if !ok {
				return nil, command.Namespace{}, 0, fmt.Errorf("firstBatch should be an array but it is a BSON %s", elem.Value().Type)
			}

			batch.Style = bsoncore.ArrayStyle
			batch.Data = arr
		case "ns":
			if elem.Value().Type != bson.TypeString {
				return nil, command.Namespace{}, 0, fmt.Errorf("namespace should be a string but it is a BSON %s", elem.Value().Type)
			}
			namespace = command.ParseNamespace(elem.Value().StringValue())
			err = namespace.Validate()
			if err != nil {
				return nil, command.Namespace{}, 0, err
			}
		case "id":
			cursorID, ok = elem.Value().Int64OK()
			if !ok {
				return nil, command.Namespace{}, 0, fmt.Errorf("id should be an int64 but it is a BSON %s", elem.Value().Type)
			}
		}
	}

	return batch, namespace, cursorID, nil
}
