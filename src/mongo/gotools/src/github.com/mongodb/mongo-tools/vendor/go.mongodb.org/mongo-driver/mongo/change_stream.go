// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"context"
	"errors"
	"time"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
)

const errorInterrupted int32 = 11601
const errorCappedPositionLost int32 = 136
const errorCursorKilled int32 = 237

// ErrMissingResumeToken indicates that a change stream notification from the server did not
// contain a resume token.
var ErrMissingResumeToken = errors.New("cannot provide resume functionality when the resume token is missing")

// ErrNilCursor indicates that the cursor for the change stream is nil.
var ErrNilCursor = errors.New("cursor is nil")

// ChangeStream instances iterate a stream of change documents. Each document can be decoded via the
// Decode method. Resume tokens should be retrieved via the ResumeToken method and can be stored to
// resume the change stream at a specific point in time.
//
// A typical usage of the ChangeStream type would be:
type ChangeStream struct {
	// Current is the BSON bytes of the current change document. This property is only valid until
	// the next call to Next or Close. If continued access is required to the bson.Raw, you must
	// make a copy of it.
	Current bson.Raw

	cmd         bsonx.Doc // aggregate command to run to create stream and rebuild cursor
	pipeline    bsonx.Arr
	options     *options.ChangeStreamOptions
	coll        *Collection
	db          *Database
	ns          command.Namespace
	cursor      *Cursor
	cursorOpts  bsonx.Doc
	getMoreOpts bsonx.Doc

	resumeToken bsonx.Doc
	err         error
	streamType  StreamType
	client      *Client
	sess        Session
	readPref    *readpref.ReadPref
	readConcern *readconcern.ReadConcern
	registry    *bsoncodec.Registry
}

func (cs *ChangeStream) replaceOptions(desc description.SelectedServer) {
	// if cs has not received any changes and resumeAfter not specified and max wire version >= 7, run known agg cmd
	// with startAtOperationTime set to startAtOperationTime provided by user or saved from initial agg
	// must not send resumeAfter key

	// else: run known agg cmd with resumeAfter set to last known resumeToken
	// must not set startAtOperationTime (remove if originally in cmd)

	if cs.options.ResumeAfter == nil && desc.WireVersion.Max >= 7 && cs.resumeToken == nil {
		cs.options.SetStartAtOperationTime(cs.sess.OperationTime())
	} else {
		if cs.resumeToken == nil {
			return // restart stream without the resume token
		}

		cs.options.SetResumeAfter(cs.resumeToken)
		// remove startAtOperationTime
		cs.options.SetStartAtOperationTime(nil)
	}
}

// Create options docs for the pipeline and cursor
func createCmdDocs(csType StreamType, opts *options.ChangeStreamOptions, registry *bsoncodec.Registry) (bsonx.Doc,
	bsonx.Doc, bsonx.Doc, bsonx.Doc, error) {

	pipelineDoc := bsonx.Doc{}
	cursorDoc := bsonx.Doc{}
	optsDoc := bsonx.Doc{}
	getMoreOptsDoc := bsonx.Doc{}

	if csType == ClientStream {
		pipelineDoc = pipelineDoc.Append("allChangesForCluster", bsonx.Boolean(true))
	}

	if opts.BatchSize != nil {
		cursorDoc = cursorDoc.Append("batchSize", bsonx.Int32(*opts.BatchSize))
	}
	if opts.Collation != nil {
		collDoc, err := bsonx.ReadDoc(opts.Collation.ToDocument())
		if err != nil {
			return nil, nil, nil, nil, err
		}
		optsDoc = optsDoc.Append("collation", bsonx.Document(collDoc))
	}
	if opts.FullDocument != nil {
		pipelineDoc = pipelineDoc.Append("fullDocument", bsonx.String(string(*opts.FullDocument)))
	}
	if opts.MaxAwaitTime != nil {
		ms := int64(time.Duration(*opts.MaxAwaitTime) / time.Millisecond)
		getMoreOptsDoc = getMoreOptsDoc.Append("maxTimeMS", bsonx.Int64(ms))
	}
	if opts.ResumeAfter != nil {
		rt, err := transformDocument(registry, opts.ResumeAfter)
		if err != nil {
			return nil, nil, nil, nil, err
		}

		pipelineDoc = pipelineDoc.Append("resumeAfter", bsonx.Document(rt))
	}
	if opts.StartAtOperationTime != nil {
		pipelineDoc = pipelineDoc.Append("startAtOperationTime",
			bsonx.Timestamp(opts.StartAtOperationTime.T, opts.StartAtOperationTime.I))
	}

	return pipelineDoc, cursorDoc, optsDoc, getMoreOptsDoc, nil
}

func getSession(ctx context.Context, client *Client) (Session, error) {
	sess := sessionFromContext(ctx)
	if err := client.validSession(sess); err != nil {
		return nil, err
	}

	var mongoSess Session
	if sess != nil {
		mongoSess = &sessionImpl{
			Client: sess,
		}
	} else {
		// create implicit session because it will be needed
		newSess, err := session.NewClientSession(client.topology.SessionPool, client.id, session.Implicit)
		if err != nil {
			return nil, err
		}

		mongoSess = &sessionImpl{
			Client: newSess,
		}
	}

	return mongoSess, nil
}

func parseOptions(csType StreamType, opts *options.ChangeStreamOptions, registry *bsoncodec.Registry) (bsonx.Doc,
	bsonx.Doc, bsonx.Doc, bsonx.Doc, error) {

	if opts.FullDocument == nil {
		opts = opts.SetFullDocument(options.Default)
	}

	pipelineDoc, cursorDoc, optsDoc, getMoreOptsDoc, err := createCmdDocs(csType, opts, registry)
	if err != nil {
		return nil, nil, nil, nil, err
	}

	return pipelineDoc, cursorDoc, optsDoc, getMoreOptsDoc, nil
}

func (cs *ChangeStream) runCommand(ctx context.Context, replaceOptions bool) error {
	ss, err := cs.client.topology.SelectServer(ctx, cs.db.writeSelector)
	if err != nil {
		return replaceErrors(err)
	}

	desc := ss.Description()
	conn, err := ss.Connection(ctx)
	if err != nil {
		return replaceErrors(err)
	}
	defer conn.Close()

	if replaceOptions {
		cs.replaceOptions(desc)
		optionsDoc, _, _, _, err := createCmdDocs(cs.streamType, cs.options, cs.registry)
		if err != nil {
			return err
		}

		changeStreamDoc := bsonx.Doc{
			{"$changeStream", bsonx.Document(optionsDoc)},
		}
		cs.pipeline[0] = bsonx.Document(changeStreamDoc)
		cs.cmd.Set("pipeline", bsonx.Array(cs.pipeline))
	}

	readCmd := command.Read{
		DB:          cs.db.name,
		Command:     cs.cmd,
		Session:     cs.sess.(*sessionImpl).Client,
		Clock:       cs.client.clock,
		ReadPref:    cs.readPref,
		ReadConcern: cs.readConcern,
	}

	rdr, err := readCmd.RoundTrip(ctx, desc, conn)
	if err != nil {
		cs.sess.EndSession(ctx)
		return replaceErrors(err)
	}

	batchCursor, err := driver.NewBatchCursor(bsoncore.Document(rdr), readCmd.Session, readCmd.Clock, ss.Server, cs.getMoreOpts...)
	if err != nil {
		cs.sess.EndSession(ctx)
		return replaceErrors(err)
	}
	cursor, err := newCursor(batchCursor, cs.registry)
	if err != nil {
		cs.sess.EndSession(ctx)
		return err
	}
	cs.cursor = cursor

	cursorValue, err := rdr.LookupErr("cursor")
	if err != nil {
		return err
	}
	cursorDoc := cursorValue.Document()
	cs.ns = command.ParseNamespace(cursorDoc.Lookup("ns").StringValue())

	return nil
}

func newChangeStream(ctx context.Context, coll *Collection, pipeline interface{},
	opts ...*options.ChangeStreamOptions) (*ChangeStream, error) {

	pipelineArr, err := transformAggregatePipeline(coll.registry, pipeline)
	if err != nil {
		return nil, err
	}

	csOpts := options.MergeChangeStreamOptions(opts...)
	pipelineDoc, cursorDoc, optsDoc, getMoreDoc, err := parseOptions(CollectionStream, csOpts, coll.registry)
	if err != nil {
		return nil, err
	}
	sess, err := getSession(ctx, coll.client)
	if err != nil {
		return nil, err
	}

	csDoc := bsonx.Document(bsonx.Doc{
		{"$changeStream", bsonx.Document(pipelineDoc)},
	})
	pipelineArr = append(bsonx.Arr{csDoc}, pipelineArr...)

	cmd := bsonx.Doc{
		{"aggregate", bsonx.String(coll.name)},
		{"pipeline", bsonx.Array(pipelineArr)},
		{"cursor", bsonx.Document(cursorDoc)},
	}
	cmd = append(cmd, optsDoc...)

	cs := &ChangeStream{
		client:      coll.client,
		sess:        sess,
		cmd:         cmd,
		pipeline:    pipelineArr,
		coll:        coll,
		db:          coll.db,
		streamType:  CollectionStream,
		readPref:    coll.readPreference,
		readConcern: coll.readConcern,
		options:     csOpts,
		registry:    coll.registry,
		cursorOpts:  cursorDoc,
		getMoreOpts: getMoreDoc,
	}

	err = cs.runCommand(ctx, false)
	if err != nil {
		return nil, err
	}

	return cs, nil
}

func newDbChangeStream(ctx context.Context, db *Database, pipeline interface{},
	opts ...*options.ChangeStreamOptions) (*ChangeStream, error) {

	pipelineArr, err := transformAggregatePipeline(db.registry, pipeline)
	if err != nil {
		return nil, err
	}

	csOpts := options.MergeChangeStreamOptions(opts...)
	pipelineDoc, cursorDoc, optsDoc, getMoreDoc, err := parseOptions(DatabaseStream, csOpts, db.registry)
	if err != nil {
		return nil, err
	}
	sess, err := getSession(ctx, db.client)
	if err != nil {
		return nil, err
	}

	csDoc := bsonx.Document(bsonx.Doc{
		{"$changeStream", bsonx.Document(pipelineDoc)},
	})
	pipelineArr = append(bsonx.Arr{csDoc}, pipelineArr...)

	cmd := bsonx.Doc{
		{"aggregate", bsonx.Int32(1)},
		{"pipeline", bsonx.Array(pipelineArr)},
		{"cursor", bsonx.Document(cursorDoc)},
	}
	cmd = append(cmd, optsDoc...)

	cs := &ChangeStream{
		client:      db.client,
		db:          db,
		sess:        sess,
		cmd:         cmd,
		pipeline:    pipelineArr,
		streamType:  DatabaseStream,
		readPref:    db.readPreference,
		readConcern: db.readConcern,
		options:     csOpts,
		registry:    db.registry,
		cursorOpts:  cursorDoc,
		getMoreOpts: getMoreDoc,
	}

	err = cs.runCommand(ctx, false)
	if err != nil {
		return nil, err
	}

	return cs, nil
}

func newClientChangeStream(ctx context.Context, client *Client, pipeline interface{},
	opts ...*options.ChangeStreamOptions) (*ChangeStream, error) {

	pipelineArr, err := transformAggregatePipeline(client.registry, pipeline)
	if err != nil {
		return nil, err
	}

	csOpts := options.MergeChangeStreamOptions(opts...)
	pipelineDoc, cursorDoc, optsDoc, getMoreDoc, err := parseOptions(ClientStream, csOpts, client.registry)
	if err != nil {
		return nil, err
	}
	sess, err := getSession(ctx, client)
	if err != nil {
		return nil, err
	}

	csDoc := bsonx.Document(bsonx.Doc{
		{"$changeStream", bsonx.Document(pipelineDoc)},
	})
	pipelineArr = append(bsonx.Arr{csDoc}, pipelineArr...)

	cmd := bsonx.Doc{
		{"aggregate", bsonx.Int32(1)},
		{"pipeline", bsonx.Array(pipelineArr)},
		{"cursor", bsonx.Document(cursorDoc)},
	}
	cmd = append(cmd, optsDoc...)

	cs := &ChangeStream{
		client:      client,
		db:          client.Database("admin"),
		sess:        sess,
		cmd:         cmd,
		pipeline:    pipelineArr,
		streamType:  ClientStream,
		readPref:    client.readPreference,
		readConcern: client.readConcern,
		options:     csOpts,
		registry:    client.registry,
		cursorOpts:  cursorDoc,
		getMoreOpts: getMoreDoc,
	}

	err = cs.runCommand(ctx, false)
	if err != nil {
		return nil, err
	}

	return cs, nil
}

func (cs *ChangeStream) storeResumeToken() error {
	idVal, err := cs.cursor.Current.LookupErr("_id")
	if err != nil {
		_ = cs.Close(context.Background())
		return ErrMissingResumeToken
	}

	var idDoc bson.Raw
	idDoc, ok := idVal.DocumentOK()
	if !ok {
		_ = cs.Close(context.Background())
		return ErrMissingResumeToken
	}
	tokenDoc, err := bsonx.ReadDoc(idDoc)
	if err != nil {
		_ = cs.Close(context.Background())
		return ErrMissingResumeToken
	}

	cs.resumeToken = tokenDoc
	return nil
}

// ID returns the cursor ID for this change stream.
func (cs *ChangeStream) ID() int64 {
	if cs.cursor == nil {
		return 0
	}

	return cs.cursor.ID()
}

// Next gets the next result from this change stream. Returns true if there were no errors and the next
// result is available for decoding.
func (cs *ChangeStream) Next(ctx context.Context) bool {
	// execute in a loop to retry resume-able errors and advance the underlying cursor
	for {
		if cs.cursor == nil {
			return false
		}

		if cs.cursor.Next(ctx) {
			err := cs.storeResumeToken()
			if err != nil {
				cs.err = err
				return false
			}

			cs.Current = cs.cursor.Current
			return true
		}

		err := cs.cursor.Err()
		if err == nil {
			return false
		}

		switch t := err.(type) {
		case command.Error:
			if t.Code == errorInterrupted || t.Code == errorCappedPositionLost || t.Code == errorCursorKilled {
				return false
			}
		}

		_, _ = driver.KillCursors(ctx, cs.ns, cs.cursor.bc.Server(), cs.ID())

		cs.err = cs.runCommand(ctx, true)
		if cs.err != nil {
			return false
		}
	}
}

// Decode will decode the current document into val.
func (cs *ChangeStream) Decode(out interface{}) error {
	if cs.cursor == nil {
		return ErrNilCursor
	}

	return bson.UnmarshalWithRegistry(cs.registry, cs.Current, out)
}

// Err returns the current error.
func (cs *ChangeStream) Err() error {
	if cs.err != nil {
		return replaceErrors(cs.err)
	}
	if cs.cursor == nil {
		return nil
	}

	return cs.cursor.Err()
}

// Close closes this cursor.
func (cs *ChangeStream) Close(ctx context.Context) error {
	if cs.cursor == nil {
		return nil // cursor is already closed
	}

	return replaceErrors(cs.cursor.Close(ctx))
}

// StreamType represents the type of a change stream.
type StreamType uint8

// These constants represent valid change stream types. A change stream can be initialized over a collection, all
// collections in a database, or over a whole client.
const (
	CollectionStream StreamType = iota
	DatabaseStream
	ClientStream
)
