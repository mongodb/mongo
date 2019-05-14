// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongo

import (
	"context"

	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
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

func (db *Database) processRunCommand(ctx context.Context, cmd interface{}, opts ...*options.RunCmdOptions) (command.Read,
	description.ServerSelector, error) {

	if ctx == nil {
		ctx = context.Background()
	}

	sess := sessionFromContext(ctx)
	runCmd := options.MergeRunCmdOptions(opts...)

	if err := db.client.validSession(sess); err != nil {
		return command.Read{}, nil, err
	}

	rp := runCmd.ReadPreference
	if rp == nil {
		if sess != nil && sess.TransactionRunning() {
			rp = sess.CurrentRp // override with transaction read pref if specified
		}
		if rp == nil {
			rp = readpref.Primary() // set to primary if nothing specified in options
		}
	}

	runCmdDoc, err := transformDocument(db.registry, cmd)
	if err != nil {
		return command.Read{}, nil, err
	}

	readSelect := description.CompositeSelector([]description.ServerSelector{
		description.ReadPrefSelector(rp),
		description.LatencySelector(db.client.localThreshold),
	})

	return command.Read{
		DB:       db.Name(),
		Command:  runCmdDoc,
		ReadPref: rp,
		Session:  sess,
		Clock:    db.client.clock,
	}, readSelect, nil
}

// RunCommand runs a command on the database. A user can supply a custom
// context to this method, or nil to default to context.Background().
func (db *Database) RunCommand(ctx context.Context, runCommand interface{}, opts ...*options.RunCmdOptions) *SingleResult {
	if ctx == nil {
		ctx = context.Background()
	}

	readCmd, readSelect, err := db.processRunCommand(ctx, runCommand, opts...)
	if err != nil {
		return &SingleResult{err: err}
	}

	doc, err := driverlegacy.Read(ctx,
		readCmd,
		db.client.topology,
		readSelect,
		db.client.id,
		db.client.topology.SessionPool,
	)

	return &SingleResult{err: replaceErrors(err), rdr: doc, reg: db.registry}
}

// RunCommandCursor runs a command on the database and returns a cursor over the resulting reader. A user can supply
// a custom context to this method, or nil to default to context.Background().
func (db *Database) RunCommandCursor(ctx context.Context, runCommand interface{}, opts ...*options.RunCmdOptions) (*Cursor, error) {
	if ctx == nil {
		ctx = context.Background()
	}

	readCmd, readSelect, err := db.processRunCommand(ctx, runCommand, opts...)
	if err != nil {
		return nil, err
	}

	batchCursor, err := driverlegacy.ReadCursor(
		ctx,
		readCmd,
		db.client.topology,
		readSelect,
		db.client.id,
		db.client.topology.SessionPool,
	)
	if err != nil {
		return nil, replaceErrors(err)
	}

	cursor, err := newCursor(batchCursor, db.registry)
	return cursor, replaceErrors(err)
}

// Drop drops this database from mongodb.
func (db *Database) Drop(ctx context.Context) error {
	if ctx == nil {
		ctx = context.Background()
	}

	sess := sessionFromContext(ctx)

	err := db.client.validSession(sess)
	if err != nil {
		return err
	}

	cmd := command.DropDatabase{
		DB:      db.name,
		Session: sess,
		Clock:   db.client.clock,
	}
	_, err = driverlegacy.DropDatabase(
		ctx, cmd,
		db.client.topology,
		db.writeSelector,
		db.client.id,
		db.client.topology.SessionPool,
	)
	if err != nil && !command.IsNotFound(err) {
		return replaceErrors(err)
	}
	return nil
}

// ListCollections list collections from mongodb database.
func (db *Database) ListCollections(ctx context.Context, filter interface{}, opts ...*options.ListCollectionsOptions) (*Cursor, error) {
	if ctx == nil {
		ctx = context.Background()
	}

	sess := sessionFromContext(ctx)

	err := db.client.validSession(sess)
	if err != nil {
		return nil, err
	}

	filterDoc, err := transformDocument(db.registry, filter)
	if err != nil {
		return nil, err
	}

	cmd := command.ListCollections{
		DB:       db.name,
		Filter:   filterDoc,
		ReadPref: readpref.Primary(), // list collections must be run on a primary by default
		Session:  sess,
		Clock:    db.client.clock,
	}

	readSelector := description.CompositeSelector([]description.ServerSelector{
		description.ReadPrefSelector(readpref.Primary()),
		description.LatencySelector(db.client.localThreshold),
	})
	batchCursor, err := driverlegacy.ListCollections(
		ctx, cmd,
		db.client.topology,
		readSelector,
		db.client.id,
		db.client.topology.SessionPool,
		opts...,
	)
	if err != nil {
		return nil, replaceErrors(err)
	}

	cursor, err := newCursor(batchCursor, db.registry)
	return cursor, replaceErrors(err)
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

	return newDbChangeStream(ctx, db, pipeline, opts...)
}
