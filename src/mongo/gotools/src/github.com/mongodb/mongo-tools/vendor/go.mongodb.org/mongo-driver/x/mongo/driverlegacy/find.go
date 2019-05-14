// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package driverlegacy

import (
	"context"

	"time"

	"errors"

	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/connection"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// Find handles the full cycle dispatch and execution of a find command against the provided
// topology.
func Find(
	ctx context.Context,
	cmd command.Find,
	topo *topology.Topology,
	selector description.ServerSelector,
	clientID uuid.UUID,
	pool *session.Pool,
	registry *bsoncodec.Registry,
	opts ...*options.FindOptions,
) (*BatchCursor, error) {

	if cmd.Session != nil && cmd.Session.PinnedServer != nil {
		selector = cmd.Session.PinnedServer
	}
	ss, err := topo.SelectServer(ctx, selector)
	if err != nil {
		return nil, err
	}

	desc := ss.Description()
	conn, err := ss.Connection(ctx)
	if err != nil {
		return nil, err
	}
	defer conn.Close()

	if desc.WireVersion.Max < 4 {
		return legacyFind(ctx, cmd, registry, ss, conn, opts...)
	}

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

	fo := options.MergeFindOptions(opts...)
	if fo.AllowPartialResults != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"allowPartialResults", bsonx.Boolean(*fo.AllowPartialResults)})
	}
	if fo.BatchSize != nil {
		elem := bsonx.Elem{"batchSize", bsonx.Int32(*fo.BatchSize)}
		cmd.Opts = append(cmd.Opts, elem)
		cmd.CursorOpts = append(cmd.CursorOpts, elem)

		if fo.Limit != nil && *fo.BatchSize != 0 && *fo.Limit <= int64(*fo.BatchSize) {
			cmd.Opts = append(cmd.Opts, bsonx.Elem{"singleBatch", bsonx.Boolean(true)})
		}
	}
	if fo.Collation != nil {
		if desc.WireVersion.Max < 5 {
			return nil, ErrCollation
		}
		collDoc, err := bsonx.ReadDoc(fo.Collation.ToDocument())
		if err != nil {
			return nil, err
		}
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"collation", bsonx.Document(collDoc)})
	}
	if fo.Comment != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"comment", bsonx.String(*fo.Comment)})
	}
	if fo.CursorType != nil {
		switch *fo.CursorType {
		case options.Tailable:
			cmd.Opts = append(cmd.Opts, bsonx.Elem{"tailable", bsonx.Boolean(true)})
		case options.TailableAwait:
			cmd.Opts = append(cmd.Opts, bsonx.Elem{"tailable", bsonx.Boolean(true)}, bsonx.Elem{"awaitData", bsonx.Boolean(true)})
		}
	}
	if fo.Hint != nil {
		hintElem, err := interfaceToElement("hint", fo.Hint, registry)
		if err != nil {
			return nil, err
		}

		cmd.Opts = append(cmd.Opts, hintElem)
	}
	if fo.Limit != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"limit", bsonx.Int64(*fo.Limit)})
	}
	if fo.Max != nil {
		maxElem, err := interfaceToElement("max", fo.Max, registry)
		if err != nil {
			return nil, err
		}

		cmd.Opts = append(cmd.Opts, maxElem)
	}
	if fo.MaxAwaitTime != nil {
		// Specified as maxTimeMS on the in the getMore command and not given in initial find command.
		cmd.CursorOpts = append(cmd.CursorOpts, bsonx.Elem{"maxTimeMS", bsonx.Int64(int64(*fo.MaxAwaitTime / time.Millisecond))})
	}
	if fo.MaxTime != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"maxTimeMS", bsonx.Int64(int64(*fo.MaxTime / time.Millisecond))})
	}
	if fo.Min != nil {
		minElem, err := interfaceToElement("min", fo.Min, registry)
		if err != nil {
			return nil, err
		}

		cmd.Opts = append(cmd.Opts, minElem)
	}
	if fo.NoCursorTimeout != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"noCursorTimeout", bsonx.Boolean(*fo.NoCursorTimeout)})
	}
	if fo.OplogReplay != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"oplogReplay", bsonx.Boolean(*fo.OplogReplay)})
	}
	if fo.Projection != nil {
		projElem, err := interfaceToElement("projection", fo.Projection, registry)
		if err != nil {
			return nil, err
		}

		cmd.Opts = append(cmd.Opts, projElem)
	}
	if fo.ReturnKey != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"returnKey", bsonx.Boolean(*fo.ReturnKey)})
	}
	if fo.ShowRecordID != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"showRecordId", bsonx.Boolean(*fo.ShowRecordID)})
	}
	if fo.Skip != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"skip", bsonx.Int64(*fo.Skip)})
	}
	if fo.Snapshot != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"snapshot", bsonx.Boolean(*fo.Snapshot)})
	}
	if fo.Sort != nil {
		sortElem, err := interfaceToElement("sort", fo.Sort, registry)
		if err != nil {
			return nil, err
		}

		cmd.Opts = append(cmd.Opts, sortElem)
	}

	res, err := cmd.RoundTrip(ctx, desc, conn)
	if err != nil {
		closeImplicitSession(cmd.Session)
		return nil, err
	}

	return NewBatchCursor(bsoncore.Document(res), cmd.Session, cmd.Clock, ss.Server, cmd.CursorOpts...)
}

// legacyFind handles the dispatch and execution of a find operation against a pre-3.2 server.
func legacyFind(
	ctx context.Context,
	cmd command.Find,
	registry *bsoncodec.Registry,
	ss *topology.SelectedServer,
	conn connection.Connection,
	opts ...*options.FindOptions,
) (*BatchCursor, error) {
	query := wiremessage.Query{
		FullCollectionName: cmd.NS.DB + "." + cmd.NS.Collection,
	}

	fo := options.MergeFindOptions(opts...)
	optsDoc, err := createLegacyOptionsDoc(fo, registry)
	if err != nil {
		return nil, err
	}
	if fo.Projection != nil {
		projDoc, err := interfaceToDocument(fo.Projection, registry)
		if err != nil {
			return nil, err
		}

		projRaw, err := projDoc.MarshalBSON()
		if err != nil {
			return nil, err
		}
		query.ReturnFieldsSelector = projRaw
	}
	if fo.Skip != nil {
		query.NumberToSkip = int32(*fo.Skip)
		query.SkipSet = true
	}
	// batch size of 1 not possible with OP_QUERY because the cursor will be closed immediately
	if fo.BatchSize != nil && *fo.BatchSize == 1 {
		query.NumberToReturn = 2
	} else {
		query.NumberToReturn = calculateNumberToReturn(fo)
	}
	query.Flags = calculateLegacyFlags(fo)

	query.BatchSize = fo.BatchSize
	if fo.Limit != nil {
		i := int32(*fo.Limit)
		query.Limit = &i
	}

	// set read preference and/or slaveOK flag
	desc := ss.SelectedDescription()
	if slaveOkNeeded(cmd.ReadPref, desc) {
		query.Flags |= wiremessage.SlaveOK
	}
	optsDoc = addReadPref(cmd.ReadPref, desc.Server.Kind, optsDoc)

	if cmd.Filter == nil {
		cmd.Filter = bsonx.Doc{}
	}

	// filter must be wrapped in $query if other $modifiers are used
	var queryDoc bsonx.Doc
	if len(optsDoc) == 0 {
		queryDoc = cmd.Filter
	} else {
		filterDoc := bsonx.Doc{
			{"$query", bsonx.Document(cmd.Filter)},
		}
		// $query should go first
		queryDoc = append(filterDoc, optsDoc...)
	}

	queryRaw, err := queryDoc.MarshalBSON()
	if err != nil {
		return nil, err
	}
	query.Query = queryRaw

	reply, err := roundTripQuery(ctx, query, conn)
	if err != nil {
		return nil, err
	}

	var cursorLimit int32
	var cursorBatchSize int32
	if query.Limit != nil {
		cursorLimit = int32(*query.Limit)
		if cursorLimit < 0 {
			cursorLimit *= -1
		}
	}
	if query.BatchSize != nil {
		cursorBatchSize = int32(*query.BatchSize)
	}

	// TODO(GODRIVER-617): When the wiremessage package is updated, we should ensure we can get all
	// of the documents as a single slice instead of having to reslice.
	ds := new(bsoncore.DocumentSequence)
	ds.Style = bsoncore.SequenceStyle
	for _, doc := range reply.Documents {
		ds.Data = append(ds.Data, doc...)
	}

	return NewLegacyBatchCursor(cmd.NS, reply.CursorID, ds, cursorLimit, cursorBatchSize, ss.Server)
}

func createLegacyOptionsDoc(fo *options.FindOptions, registry *bsoncodec.Registry) (bsonx.Doc, error) {
	var optsDoc bsonx.Doc

	if fo.Collation != nil {
		return nil, ErrCollation
	}
	if fo.Comment != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"$comment", bsonx.String(*fo.Comment)})
	}
	if fo.Hint != nil {
		hintElem, err := interfaceToElement("$hint", fo.Hint, registry)
		if err != nil {
			return nil, err
		}

		optsDoc = append(optsDoc, hintElem)
	}
	if fo.Max != nil {
		maxElem, err := interfaceToElement("$max", fo.Max, registry)
		if err != nil {
			return nil, err
		}

		optsDoc = append(optsDoc, maxElem)
	}
	if fo.MaxTime != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"$maxTimeMS", bsonx.Int64(int64(*fo.MaxTime / time.Millisecond))})
	}
	if fo.Min != nil {
		minElem, err := interfaceToElement("$min", fo.Min, registry)
		if err != nil {
			return nil, err
		}

		optsDoc = append(optsDoc, minElem)
	}
	if fo.ReturnKey != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"$returnKey", bsonx.Boolean(*fo.ReturnKey)})
	}
	if fo.ShowRecordID != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"$showDiskLoc", bsonx.Boolean(*fo.ShowRecordID)})
	}
	if fo.Snapshot != nil {
		optsDoc = append(optsDoc, bsonx.Elem{"$snapshot", bsonx.Boolean(*fo.Snapshot)})
	}
	if fo.Sort != nil {
		sortElem, err := interfaceToElement("$orderby", fo.Sort, registry)
		if err != nil {
			return nil, err
		}

		optsDoc = append(optsDoc, sortElem)
	}

	return optsDoc, nil
}

func calculateLegacyFlags(fo *options.FindOptions) wiremessage.QueryFlag {
	var flags wiremessage.QueryFlag

	if fo.AllowPartialResults != nil {
		flags |= wiremessage.Partial
	}
	if fo.CursorType != nil {
		switch *fo.CursorType {
		case options.Tailable:
			flags |= wiremessage.TailableCursor
		case options.TailableAwait:
			flags |= wiremessage.TailableCursor
			flags |= wiremessage.AwaitData
		}
	}
	if fo.NoCursorTimeout != nil {
		flags |= wiremessage.NoCursorTimeout
	}
	if fo.OplogReplay != nil {
		flags |= wiremessage.OplogReplay
	}

	return flags
}

// calculate the number to return for the first find query
func calculateNumberToReturn(opts *options.FindOptions) int32 {
	var numReturn int32
	var limit int32
	var batchSize int32

	if opts.Limit != nil {
		limit = int32(*opts.Limit)
	}
	if opts.BatchSize != nil {
		batchSize = int32(*opts.BatchSize)
	}

	if limit < 0 {
		numReturn = limit
	} else if limit == 0 {
		numReturn = batchSize
	} else if limit < batchSize {
		numReturn = limit
	} else {
		numReturn = batchSize
	}

	return numReturn
}

func slaveOkNeeded(rp *readpref.ReadPref, desc description.SelectedServer) bool {
	if desc.Kind == description.Single && desc.Server.Kind != description.Mongos {
		return true
	}
	if rp == nil {
		// assume primary
		return false
	}

	return rp.Mode() != readpref.PrimaryMode
}

func addReadPref(rp *readpref.ReadPref, kind description.ServerKind, query bsonx.Doc) bsonx.Doc {
	if !readPrefNeeded(rp, kind) {
		return query
	}

	doc := createReadPref(rp)
	if doc == nil {
		return query
	}

	return query.Append("$readPreference", bsonx.Document(doc))
}

func readPrefNeeded(rp *readpref.ReadPref, kind description.ServerKind) bool {
	if kind != description.Mongos || rp == nil {
		return false
	}

	// simple Primary or SecondaryPreferred is communicated via slaveOk to Mongos.
	if rp.Mode() == readpref.PrimaryMode || rp.Mode() == readpref.SecondaryPreferredMode {
		if _, ok := rp.MaxStaleness(); !ok && len(rp.TagSets()) == 0 {
			return false
		}
	}

	return true
}

func createReadPref(rp *readpref.ReadPref) bsonx.Doc {
	if rp == nil {
		return nil
	}

	doc := bsonx.Doc{}

	switch rp.Mode() {
	case readpref.PrimaryMode:
		doc = append(doc, bsonx.Elem{"mode", bsonx.String("primary")})
	case readpref.PrimaryPreferredMode:
		doc = append(doc, bsonx.Elem{"mode", bsonx.String("primaryPreferred")})
	case readpref.SecondaryPreferredMode:
		doc = append(doc, bsonx.Elem{"mode", bsonx.String("secondaryPreferred")})
	case readpref.SecondaryMode:
		doc = append(doc, bsonx.Elem{"mode", bsonx.String("secondary")})
	case readpref.NearestMode:
		doc = append(doc, bsonx.Elem{"mode", bsonx.String("nearest")})
	}

	sets := make([]bsonx.Val, 0, len(rp.TagSets()))
	for _, ts := range rp.TagSets() {
		if len(ts) == 0 {
			continue
		}
		set := bsonx.Doc{}
		for _, t := range ts {
			set = append(set, bsonx.Elem{t.Name, bsonx.String(t.Value)})
		}
		sets = append(sets, bsonx.Document(set))
	}
	if len(sets) > 0 {
		doc = append(doc, bsonx.Elem{"tags", bsonx.Array(sets)})
	}
	if d, ok := rp.MaxStaleness(); ok {
		doc = append(doc, bsonx.Elem{"maxStalenessSeconds", bsonx.Int32(int32(d.Seconds()))})
	}

	return doc
}

func roundTripQuery(ctx context.Context, query wiremessage.Query, conn connection.Connection) (wiremessage.Reply, error) {
	err := conn.WriteWireMessage(ctx, query)
	if err != nil {
		if _, ok := err.(command.Error); ok {
			return wiremessage.Reply{}, err
		}
		return wiremessage.Reply{}, command.Error{
			Message: err.Error(),
			Labels:  []string{command.NetworkError},
		}
	}

	wm, err := conn.ReadWireMessage(ctx)
	if err != nil {
		if _, ok := err.(command.Error); ok {
			return wiremessage.Reply{}, err
		}
		// Connection errors are transient
		return wiremessage.Reply{}, command.Error{
			Message: err.Error(),
			Labels:  []string{command.NetworkError},
		}
	}

	reply, ok := wm.(wiremessage.Reply)
	if !ok {
		return wiremessage.Reply{}, errors.New("did not receive OP_REPLY response")
	}

	err = validateOpReply(reply)
	if err != nil {
		return wiremessage.Reply{}, err
	}

	return reply, nil
}

func validateOpReply(reply wiremessage.Reply) error {
	if int(reply.NumberReturned) != len(reply.Documents) {
		return command.NewCommandResponseError(command.ReplyDocumentMismatch, nil)
	}

	if reply.ResponseFlags&wiremessage.QueryFailure == wiremessage.QueryFailure {
		return command.QueryFailureError{
			Message:  "query failure",
			Response: reply.Documents[0],
		}
	}

	return nil
}
