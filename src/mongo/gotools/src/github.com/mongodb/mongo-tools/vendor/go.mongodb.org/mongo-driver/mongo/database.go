// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"context"
	"errors"
	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/mongo/driver/description"
	"go.mongodb.org/mongo-driver/x/mongo/driver/operation"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
)

var (
	defaultRunCmdOpts = []*options.RunCmdOptions{options.RunCmd().SetReadPreference(readpref.Primary())}
)

// Database performs operations on a given database.
type Database struct {
	client         *Client
	name           string
	readConcern    *readconcern.ReadConcern
	writeConcern   *writeconcern.WriteConcern
	readPreference *readpref.ReadPref
	readSelector   description.ServerSelector
	writeSelector  description.ServerSelector
	registry       *bsoncodec.Registry
}

func newDatabase(client *Client, name string, opts ...*options.DatabaseOptions) *Database {
	dbOpt := options.MergeDatabaseOptions(opts...)

	rc := client.readConcern
	if dbOpt.ReadConcern != nil {
		rc = dbOpt.ReadConcern
	}

	rp := client.readPreference
	if dbOpt.ReadPreference != nil {
		rp = dbOpt.ReadPreference
	}

	wc := client.writeConcern
	if dbOpt.WriteConcern != nil {
		wc = dbOpt.WriteConcern
	}

	db := &Database{
		client:         client,
		name:           name,
		readPreference: rp,
		readConcern:    rc,
		writeConcern:   wc,
		registry:       client.registry,
	}

	db.readSelector = description.CompositeSelector([]description.ServerSelector{
		description.ReadPrefSelector(db.readPreference),
		description.LatencySelector(db.client.localThreshold),
	})

	db.writeSelector = description.CompositeSelector([]description.ServerSelector{
		description.WriteSelector(),
		description.LatencySelector(db.client.localThreshold),
	})

	return db
}

// Client returns the Client the database was created from.
func (db *Database) Client() *Client {
	return db.client
}

// Name returns the name of the database.
func (db *Database) Name() string {
	return db.name
}

// Collection gets a handle for a given collection in the database.
func (db *Database) Collection(name string, opts ...*options.CollectionOptions) *Collection {
	return newCollection(db, name, opts...)
}

// Aggregate runs an aggregation framework pipeline.
//
// See https://docs.mongodb.com/manual/aggregation/.
func (db *Database) Aggregate(ctx context.Context, pipeline interface{},
	opts ...*options.AggregateOptions) (*Cursor, error) {
	a := aggregateParams{
		ctx:            ctx,
		pipeline:       pipeline,
		client:         db.client,
		registry:       db.registry,
		readConcern:    db.readConcern,
		writeConcern:   db.writeConcern,
		retryRead:      db.client.retryReads,
		db:             db.name,
		readSelector:   db.readSelector,
		writeSelector:  db.writeSelector,
		readPreference: db.readPreference,
		opts:           opts,
	}
	return aggregate(a)
}

func (db *Database) processRunCommand(ctx context.Context, cmd interface{},
	opts ...*options.RunCmdOptions) (*operation.Command, *session.Client, error) {
	sess := sessionFromContext(ctx)
	if sess == nil && db.client.topology.SessionPool != nil {
		var err error
		sess, err = session.NewClientSession(db.client.topology.SessionPool, db.client.id, session.Implicit)
		if err != nil {
			return nil, sess, err
		}
	}

	err := db.client.validSession(sess)
	if err != nil {
		return nil, sess, err
	}

	ro := options.MergeRunCmdOptions(append(defaultRunCmdOpts, opts...)...)
	if sess != nil && sess.TransactionRunning() && ro.ReadPreference != nil && ro.ReadPreference.Mode() != readpref.PrimaryMode {
		return nil, sess, errors.New("read preference in a transaction must be primary")
	}

	runCmdDoc, err := transformBsoncoreDocument(db.registry, cmd)
	if err != nil {
		return nil, sess, err
	}
	readSelect := description.CompositeSelector([]description.ServerSelector{
		description.ReadPrefSelector(ro.ReadPreference),
		description.LatencySelector(db.client.localThreshold),
	})
	if sess != nil && sess.PinnedServer != nil {
		readSelect = sess.PinnedServer
	}

	return operation.NewCommand(runCmdDoc).
		Session(sess).CommandMonitor(db.client.monitor).
		ServerSelector(readSelect).ClusterClock(db.client.clock).
		Database(db.name).Deployment(db.client.topology).ReadConcern(db.readConcern), sess, nil
}

// RunCommand runs a command on the database. A user can supply a custom
// context to this method, or nil to default to context.Background().
func (db *Database) RunCommand(ctx context.Context, runCommand interface{}, opts ...*options.RunCmdOptions) *SingleResult {
	if ctx == nil {
		ctx = context.Background()
	}

	op, sess, err := db.processRunCommand(ctx, runCommand, opts...)
	defer closeImplicitSession(sess)
	if err != nil {
		return &SingleResult{err: err}
	}

	err = op.Execute(ctx)
	return &SingleResult{
		err: replaceErrors(err),
		rdr: bson.Raw(op.Result()),
		reg: db.registry,
	}
}

// RunCommandCursor runs a command on the database and returns a cursor over the resulting reader. A user can supply
// a custom context to this method, or nil to default to context.Background().
func (db *Database) RunCommandCursor(ctx context.Context, runCommand interface{}, opts ...*options.RunCmdOptions) (*Cursor, error) {
	if ctx == nil {
		ctx = context.Background()
	}

	op, sess, err := db.processRunCommand(ctx, runCommand, opts...)
	if err != nil {
		closeImplicitSession(sess)
		return nil, replaceErrors(err)
	}

	if err = op.Execute(ctx); err != nil {
		closeImplicitSession(sess)
		return nil, replaceErrors(err)
	}

	bc, err := op.ResultCursor(driver.CursorOptions{})
	if err != nil {
		closeImplicitSession(sess)
		return nil, replaceErrors(err)
	}
	cursor, err := newCursorWithSession(bc, db.registry, sess)
	return cursor, replaceErrors(err)
}

// Drop drops this database from mongodb.
func (db *Database) Drop(ctx context.Context) error {
	if ctx == nil {
		ctx = context.Background()
	}

	sess := sessionFromContext(ctx)
	if sess == nil && db.client.topology.SessionPool != nil {
		sess, err := session.NewClientSession(db.client.topology.SessionPool, db.client.id, session.Implicit)
		if err != nil {
			return err
		}
		defer sess.EndSession()
	}

	err := db.client.validSession(sess)
	if err != nil {
		return err
	}

	wc := db.writeConcern
	if sess.TransactionRunning() {
		wc = nil
	}
	if !writeconcern.AckWrite(wc) {
		sess = nil
	}

	selector := makePinnedSelector(sess, db.writeSelector)

	op := operation.NewDropDatabase().
		Session(sess).WriteConcern(wc).CommandMonitor(db.client.monitor).
		ServerSelector(selector).ClusterClock(db.client.clock).
		Database(db.name).Deployment(db.client.topology)

	err = op.Execute(ctx)

	driverErr, ok := err.(driver.Error)
	if err != nil && (!ok || !driverErr.NamespaceNotFound()) {
		return replaceErrors(err)
	}
	return nil
}

// ListCollections returns a cursor over the collections in a database.
func (db *Database) ListCollections(ctx context.Context, filter interface{}, opts ...*options.ListCollectionsOptions) (*Cursor, error) {
	if ctx == nil {
		ctx = context.Background()
	}

	filterDoc, err := transformBsoncoreDocument(db.registry, filter)
	if err != nil {
		return nil, err
	}

	sess := sessionFromContext(ctx)
	if sess == nil && db.client.topology.SessionPool != nil {
		sess, err = session.NewClientSession(db.client.topology.SessionPool, db.client.id, session.Implicit)
		if err != nil {
			return nil, err
		}
	}

	err = db.client.validSession(sess)
	if err != nil {
		closeImplicitSession(sess)
		return nil, err
	}

	selector := makePinnedSelector(sess, db.readSelector)

	lco := options.MergeListCollectionsOptions(opts...)
	op := operation.NewListCollections(filterDoc).
		Session(sess).ReadPreference(db.readPreference).CommandMonitor(db.client.monitor).
		ServerSelector(selector).ClusterClock(db.client.clock).
		Database(db.name).Deployment(db.client.topology)
	if lco.NameOnly != nil {
		op = op.NameOnly(*lco.NameOnly)
	}
	retry := driver.RetryNone
	if db.client.retryReads {
		retry = driver.RetryOncePerCommand
	}
	op = op.Retry(retry)

	err = op.Execute(ctx)
	if err != nil {
		closeImplicitSession(sess)
		return nil, replaceErrors(err)
	}

	bc, err := op.Result(driver.CursorOptions{})
	if err != nil {
		closeImplicitSession(sess)
		return nil, replaceErrors(err)
	}
	cursor, err := newCursorWithSession(bc, db.registry, sess)
	return cursor, replaceErrors(err)
}

// ListCollectionNames returns a slice containing the names of all of the collections on the server.
func (db *Database) ListCollectionNames(ctx context.Context, filter interface{}, opts ...*options.ListCollectionsOptions) ([]string, error) {
	opts = append(opts, options.ListCollections().SetNameOnly(true))

	res, err := db.ListCollections(ctx, filter, opts...)
	if err != nil {
		return nil, err
	}

	names := make([]string, 0)
	for res.Next(ctx) {
		next := &bsonx.Doc{}
		err = res.Decode(next)
		if err != nil {
			return nil, err
		}

		elem, err := next.LookupErr("name")
		if err != nil {
			return nil, err
		}

		if elem.Type() != bson.TypeString {
			return nil, fmt.Errorf("incorrect type for 'name'. got %v. want %v", elem.Type(), bson.TypeString)
		}

		elemName := elem.StringValue()
		names = append(names, elemName)
	}

	return names, nil
}

// ReadConcern returns the read concern of this database.
func (db *Database) ReadConcern() *readconcern.ReadConcern {
	return db.readConcern
}

// ReadPreference returns the read preference of this database.
func (db *Database) ReadPreference() *readpref.ReadPref {
	return db.readPreference
}

// WriteConcern returns the write concern of this database.
func (db *Database) WriteConcern() *writeconcern.WriteConcern {
	return db.writeConcern
}

// Watch returns a change stream cursor used to receive information of changes to the database. This method is preferred
// to running a raw aggregation with a $changeStream stage because it supports resumability in the case of some errors.
// The database must have read concern majority or no read concern for a change stream to be created successfully.
func (db *Database) Watch(ctx context.Context, pipeline interface{},
	opts ...*options.ChangeStreamOptions) (*ChangeStream, error) {

	csConfig := changeStreamConfig{
		readConcern:    db.readConcern,
		readPreference: db.readPreference,
		client:         db.client,
		registry:       db.registry,
		streamType:     DatabaseStream,
		databaseName:   db.Name(),
	}
	return newChangeStream(ctx, csConfig, pipeline, opts...)
}
