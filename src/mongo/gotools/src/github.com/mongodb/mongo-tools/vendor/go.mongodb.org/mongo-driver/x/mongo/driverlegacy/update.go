// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package driverlegacy

import (
	"context"

	"go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/x/bsonx"

	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
)

// Update handles the full cycle dispatch and execution of an update command against the provided
// topology.
func Update(
	ctx context.Context,
	cmd command.Update,
	topo *topology.Topology,
	selector description.ServerSelector,
	clientID uuid.UUID,
	pool *session.Pool,
	retryWrite bool,
	opts ...*options.UpdateOptions,
) (result.Update, error) {

	if cmd.Session != nil && cmd.Session.PinnedServer != nil {
		selector = cmd.Session.PinnedServer
	}
	ss, err := topo.SelectServer(ctx, selector)
	if err != nil {
		return result.Update{}, err
	}

	// If no explicit session and deployment supports sessions, start implicit session.
	if cmd.Session == nil && topo.SupportsSessions() {
		cmd.Session, err = session.NewClientSession(pool, clientID, session.Implicit)
		if err != nil {
			return result.Update{}, err
		}
		defer cmd.Session.EndSession()
	}

	updateOpts := options.MergeUpdateOptions(opts...)

	if updateOpts.ArrayFilters != nil {
		if ss.Description().WireVersion.Max < 6 {
			return result.Update{}, ErrArrayFilters
		}
		filters, err := updateOpts.ArrayFilters.ToArray()
		if err != nil {
			return result.Update{}, err
		}
		arr := make(bsonx.Arr, 0, len(filters))
		for _, filter := range filters {
			doc, err := bsonx.ReadDoc(filter)
			if err != nil {
				return result.Update{}, err
			}
			arr = append(arr, bsonx.Document(doc))
		}
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"arrayFilters", bsonx.Array(arr)})
	}
	if updateOpts.BypassDocumentValidation != nil && ss.Description().WireVersion.Includes(4) {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"bypassDocumentValidation", bsonx.Boolean(*updateOpts.BypassDocumentValidation)})
	}
	if updateOpts.Collation != nil {
		if ss.Description().WireVersion.Max < 5 {
			return result.Update{}, ErrCollation
		}
		collDoc, err := bsonx.ReadDoc(updateOpts.Collation.ToDocument())
		if err != nil {
			return result.Update{}, err
		}
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"collation", bsonx.Document(collDoc)})
	}
	if updateOpts.Upsert != nil {
		cmd.Opts = append(cmd.Opts, bsonx.Elem{"upsert", bsonx.Boolean(*updateOpts.Upsert)})
	}

	// Execute in a single trip if retry writes not supported, or retry not enabled
	if !retrySupported(topo, ss.Description(), cmd.Session, cmd.WriteConcern) || !retryWrite {
		if cmd.Session != nil {
			cmd.Session.RetryWrite = false // explicitly set to false to prevent encoding transaction number
		}
		return update(ctx, &cmd, ss, nil)
	}

	cmd.Session.RetryWrite = retryWrite
	cmd.Session.IncrementTxnNumber()

	res, originalErr := update(ctx, &cmd, ss, nil)

	// Retry if appropriate
	if cerr, ok := originalErr.(command.Error); (ok && cerr.Retryable()) ||
		(res.WriteConcernError != nil && command.IsWriteConcernErrorRetryable(res.WriteConcernError)) {
		ss, err := topo.SelectServer(ctx, selector)

		// Return original error if server selection fails or new server does not support retryable writes
		if err != nil || !retrySupported(topo, ss.Description(), cmd.Session, cmd.WriteConcern) {
			return res, originalErr
		}

		return update(ctx, &cmd, ss, cerr)
	}
	return res, originalErr

}

func update(
	ctx context.Context,
	cmd *command.Update,
	ss *topology.SelectedServer,
	oldErr error,
) (result.Update, error) {
	desc := ss.Description()

	conn, err := ss.Connection(ctx)
	if err != nil {
		if oldErr != nil {
			return result.Update{}, oldErr
		}
		return result.Update{}, err
	}

	if !writeconcern.AckWrite(cmd.WriteConcern) {
		go func() {
			defer func() { _ = recover() }()
			defer conn.Close()

			_, _ = cmd.RoundTrip(ctx, desc, conn)
		}()

		return result.Update{}, command.ErrUnacknowledgedWrite
	}
	defer conn.Close()

	res, err := cmd.RoundTrip(ctx, desc, conn)
	ss.ProcessWriteConcernError(res.WriteConcernError)
	return res, err
}
