// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package driverlegacy

import (
	"context"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
)

// Read handles the full cycle dispatch and execution of a read command against the provided
// topology.
func Read(
	ctx context.Context,
	cmd command.Read,
	topo *topology.Topology,
	selector description.ServerSelector,
	clientID uuid.UUID,
	pool *session.Pool,
) (bson.Raw, error) {

	if cmd.Session != nil && cmd.Session.PinnedServer != nil {
		selector = cmd.Session.PinnedServer
	}
	ss, err := topo.SelectServer(ctx, selector)
	if err != nil {
		return nil, err
	}

	conn, err := ss.Connection(ctx)
	if err != nil {
		return nil, err
	}
	defer conn.Close()

	if cmd.Session != nil && cmd.Session.TransactionRunning() {
		// When command.read is directly used, this implies an operation level
		// read preference, so we do not override it with the transaction read pref.
		err = checkTransactionReadPref(cmd.ReadPref)

		if err != nil {
			return nil, err
		}
	}

	// If no explicit session and deployment supports sessions, start implicit session.
	if cmd.Session == nil && topo.SupportsSessions() {
		cmd.Session, err = session.NewClientSession(pool, clientID, session.Implicit)
		if err != nil {
			return nil, err
		}
		defer cmd.Session.EndSession()
	}

	return cmd.RoundTrip(ctx, ss.Description(), conn)
}

func getReadPrefBasedOnTransaction(current *readpref.ReadPref, sess *session.Client) (*readpref.ReadPref, error) {
	if sess != nil && sess.TransactionRunning() {
		// Transaction's read preference always takes priority
		current = sess.CurrentRp
		err := checkTransactionReadPref(current)
		if err != nil {
			return nil, err
		}
	}
	return current, nil
}

func checkTransactionReadPref(pref *readpref.ReadPref) error {
	if pref != nil && (pref.Mode() == readpref.SecondaryMode ||
		pref.Mode() == readpref.SecondaryPreferredMode ||
		pref.Mode() == readpref.NearestMode ||
		pref.Mode() == readpref.PrimaryPreferredMode) {
		return command.ErrNonPrimaryRP
	}
	return nil
}
