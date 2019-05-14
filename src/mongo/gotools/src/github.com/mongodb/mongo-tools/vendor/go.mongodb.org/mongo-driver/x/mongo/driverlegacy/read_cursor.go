// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package driverlegacy

import (
	"context"

	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
)

// ReadCursor handles the full dispatch cycle and execution of a read command against the provided topology and returns
// a Cursor over the resulting BSON reader.
func ReadCursor(
	ctx context.Context,
	cmd command.Read,
	topo *topology.Topology,
	selector description.ServerSelector,
	clientID uuid.UUID,
	pool *session.Pool,
	cursorOpts ...bsonx.Elem,
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

	if cmd.Session == nil && topo.SupportsSessions() {
		cmd.Session, err = session.NewClientSession(pool, clientID, session.Implicit)
		if err != nil {
			return nil, err
		}
	}

	rdr, err := cmd.RoundTrip(ctx, desc, conn)
	if err != nil {
		if cmd.Session != nil && cmd.Session.SessionType == session.Implicit {
			cmd.Session.EndSession()
		}
		return nil, err
	}

	cursor, err := NewBatchCursor(bsoncore.Document(rdr), cmd.Session, cmd.Clock, ss.Server, cursorOpts...)
	if err != nil {
		if cmd.Session != nil && cmd.Session.SessionType == session.Implicit {
			cmd.Session.EndSession()
		}
		return nil, err
	}

	return cursor, nil
}
