// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package testutil // import "go.mongodb.org/mongo-driver/internal/testutil"

import (
	"context"
	"strings"
	"testing"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/internal/testutil/helpers"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/mongo/driver/session"
	"go.mongodb.org/mongo-driver/x/mongo/driver/topology"
	"go.mongodb.org/mongo-driver/x/mongo/driver/uuid"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/description"
	"github.com/stretchr/testify/require"
)

// AutoCreateIndexes creates an index in the test cluster.
func AutoCreateIndexes(t *testing.T, keys []string) {
	indexes := bsonx.Doc{}
	for _, k := range keys {
		indexes = append(indexes, bsonx.Elem{k, bsonx.Int32(1)})
	}
	name := strings.Join(keys, "_")
	indexes = bsonx.Doc{
		{"key", bsonx.Document(indexes)},
		{"name", bsonx.String(name)},
	}
	cmd := command.CreateIndexes{
		NS:      command.NewNamespace(DBName(t), ColName(t)),
		Indexes: bsonx.Arr{bsonx.Document(indexes)},
	}
	id, _ := uuid.New()
	_, err := driver.CreateIndexes(
		context.Background(),
		cmd,
		Topology(t),
		description.WriteSelector(),
		id,
		&session.Pool{},
	)
	require.NoError(t, err)
}

// AutoDropCollection drops the collection in the test cluster.
func AutoDropCollection(t *testing.T) {
	DropCollection(t, DBName(t), ColName(t))
}

// DropCollection drops the collection in the test cluster.
func DropCollection(t *testing.T, dbname, colname string) {
	cmd := command.Write{DB: dbname, Command: bsonx.Doc{{"drop", bsonx.String(colname)}}}
	id, _ := uuid.New()
	_, err := driver.Write(
		context.Background(),
		cmd,
		Topology(t),
		description.WriteSelector(),
		id,
		&session.Pool{},
	)
	if err != nil && !command.IsNotFound(err) {
		require.NoError(t, err)
	}
}

func autoDropDB(t *testing.T, topo *topology.Topology) {
	cmd := command.Write{DB: DBName(t), Command: bsonx.Doc{{"dropDatabase", bsonx.Int32(1)}}}
	id, _ := uuid.New()
	_, err := driver.Write(
		context.Background(),
		cmd,
		topo,
		description.WriteSelector(),
		id,
		&session.Pool{},
	)
	require.NoError(t, err)
}

// AutoInsertDocs inserts the docs into the test cluster.
func AutoInsertDocs(t *testing.T, writeConcern *writeconcern.WriteConcern, docs ...bsonx.Doc) {
	InsertDocs(t, DBName(t), ColName(t), writeConcern, docs...)
}

// InsertDocs inserts the docs into the test cluster.
func InsertDocs(t *testing.T, dbname, colname string, writeConcern *writeconcern.WriteConcern, docs ...bsonx.Doc) {
	cmd := command.Insert{NS: command.NewNamespace(dbname, colname), Docs: docs}

	topo := Topology(t)
	id, _ := uuid.New()
	_, err := driver.Insert(
		context.Background(),
		cmd,
		topo,
		description.WriteSelector(),
		id,
		&session.Pool{},
		false,
	)
	require.NoError(t, err)
}

// EnableMaxTimeFailPoint turns on the max time fail point in the test cluster.
func EnableMaxTimeFailPoint(t *testing.T, s *topology.Server) error {
	cmd := command.Write{
		DB: "admin",
		Command: bsonx.Doc{
			{"configureFailPoint", bsonx.String("maxTimeAlwaysTimeOut")},
			{"mode", bsonx.String("alwaysOn")},
		},
	}
	conn, err := s.Connection(context.Background())
	require.NoError(t, err)
	defer testhelpers.RequireNoErrorOnClose(t, conn)
	_, err = cmd.RoundTrip(context.Background(), s.SelectedDescription(), conn)
	return err
}

// DisableMaxTimeFailPoint turns off the max time fail point in the test cluster.
func DisableMaxTimeFailPoint(t *testing.T, s *topology.Server) {
	cmd := command.Write{
		DB: "admin",
		Command: bsonx.Doc{
			{"configureFailPoint", bsonx.String("maxTimeAlwaysTimeOut")},
			{"mode", bsonx.String("off")},
		},
	}
	conn, err := s.Connection(context.Background())
	require.NoError(t, err)
	defer testhelpers.RequireNoErrorOnClose(t, conn)
	_, err = cmd.RoundTrip(context.Background(), s.SelectedDescription(), conn)
	require.NoError(t, err)
}

// RunCommand runs an arbitrary command on a given database of target server
func RunCommand(t *testing.T, s *topology.Server, db string, b bsonx.Doc) (bson.Raw, error) {
	conn, err := s.Connection(context.Background())
	if err != nil {
		return nil, err
	}
	defer testhelpers.RequireNoErrorOnClose(t, conn)
	cmd := command.Read{
		DB:      db,
		Command: b,
	}
	return cmd.RoundTrip(context.Background(), s.SelectedDescription(), conn)
}
