// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package driver

import (
	"context"

	"errors"

	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
	"go.mongodb.org/mongo-driver/x/mongo/driver/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driver/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/connection"
	"go.mongodb.org/mongo-driver/x/network/description"
)

// ErrFilterType is thrown when a non-string filter is specified.
var ErrFilterType = errors.New("filter must be a string")

// ListCollections handles the full cycle dispatch and execution of a listCollections command against the provided
// topology.
func ListCollections(
	ctx context.Context,
	cmd command.ListCollections,
	topo *topology.Topology,
	selector description.ServerSelector,
	clientID uuid.UUID,
	pool *session.Pool,
	opts ...*options.ListCollectionsOptions,
) (*ListCollectionsBatchCursor, error) {

	ss, err := topo.SelectServer(ctx, selector)
	if err != nil {
		return nil, err
	}

	conn, err := ss.Connection(ctx)
	if err != nil {
		return nil, err
	}
	defer conn.Close()

	if ss.Description().WireVersion.Max < 3 {
		return legacyListCollections(ctx, cmd, ss, conn)
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

	lc := options.MergeListCollectionsOptions(opts...)
	if lc.NameOnly != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"nameOnly", bsonx.Boolean(*lc.NameOnly)})
	}

	res, err := cmd.RoundTrip(ctx, ss.Description(), conn)
	if err != nil {
		closeImplicitSession(cmd.Session)
		return nil, err
	}

	batchCursor, err := NewBatchCursor(bsoncore.Document(res), cmd.Session, cmd.Clock, ss.Server, cmd.CursorOpts...)
	if err != nil {
		closeImplicitSession(cmd.Session)
		return nil, err
	}

	return NewListCollectionsBatchCursor(batchCursor)
}

func legacyListCollections(
	ctx context.Context,
	cmd command.ListCollections,
	ss *topology.SelectedServer,
	conn connection.Connection,
) (*ListCollectionsBatchCursor, error) {
	filter, err := transformFilter(cmd.Filter, cmd.DB)
	if err != nil {
		return nil, err
	}

	findCmd := command.Find{
		NS:       command.NewNamespace(cmd.DB, "system.namespaces"),
		ReadPref: cmd.ReadPref,
		Filter:   filter,
	}

	// don't need registry because it's used to create BSON docs for find options that don't exist in this case
	batchCursor, err := legacyFind(ctx, findCmd, nil, ss, conn)
	if err != nil {
		return nil, err
	}

	return NewLegacyListCollectionsBatchCursor(batchCursor)
}

// modify the user-supplied filter to prefix the "name" field with the database name.
// returns the original filter if the name field is not present or a copy with the modified name field if it is
func transformFilter(filter bsonx.Doc, dbName string) (bsonx.Doc, error) {
	if filter == nil {
		return filter, nil
	}

	if nameVal, err := filter.LookupErr("name"); err == nil {
		name, ok := nameVal.StringValueOK()
		if !ok {
			return nil, ErrFilterType
		}

		filterCopy := filter.Copy()
		filterCopy.Set("name", bsonx.String(dbName+"."+name))
		return filterCopy, nil
	}
	return filter, nil
}
