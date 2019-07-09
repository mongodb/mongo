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

	"github.com/stretchr/testify/require"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver"
	"go.mongodb.org/mongo-driver/x/mongo/driver/description"
	"go.mongodb.org/mongo-driver/x/mongo/driver/operation"
	"go.mongodb.org/mongo-driver/x/mongo/driver/topology"
)

// AutoCreateIndexes creates an index in the test cluster.
func AutoCreateIndexes(t *testing.T, keys []string) {
	var elems [][]byte
	for _, k := range keys {
		elems = append(elems, bsoncore.AppendInt32Element(nil, k, 1))
	}
	name := strings.Join(keys, "_")
	indexes := bsoncore.BuildDocumentFromElements(nil,
		bsoncore.AppendDocumentElement(nil, "key", bsoncore.BuildDocumentFromElements(nil,
			elems...)),
		bsoncore.AppendStringElement(nil, "name", name),
	)
	err := operation.NewCreateIndexes(indexes).Collection(ColName(t)).Database(DBName(t)).
		Deployment(Topology(t)).ServerSelector(description.WriteSelector()).Execute(context.Background())
	require.NoError(t, err)
}

// AutoDropCollection drops the collection in the test cluster.
func AutoDropCollection(t *testing.T) {
	DropCollection(t, DBName(t), ColName(t))
}

// DropCollection drops the collection in the test cluster.
func DropCollection(t *testing.T, dbname, colname string) {
	err := operation.NewCommand(bsoncore.BuildDocument(nil, bsoncore.AppendStringElement(nil, "drop", colname))).
		Database(dbname).ServerSelector(description.WriteSelector()).Deployment(Topology(t)).
		Execute(context.Background())
	if de, ok := err.(driver.Error); err != nil && !(ok && de.NamespaceNotFound()) {
		require.NoError(t, err)
	}
}

func autoDropDB(t *testing.T, topo *topology.Topology) {
	err := operation.NewCommand(bsoncore.BuildDocument(nil, bsoncore.AppendInt32Element(nil, "dropDatabase", 1))).
		Database(DBName(t)).ServerSelector(description.WriteSelector()).Deployment(topo).
		Execute(context.Background())
	require.NoError(t, err)
}

// AutoInsertDocs inserts the docs into the test cluster.
func AutoInsertDocs(t *testing.T, writeConcern *writeconcern.WriteConcern, docs ...bsoncore.Document) {
	InsertDocs(t, DBName(t), ColName(t), writeConcern, docs...)
}

// InsertDocs inserts the docs into the test cluster.
func InsertDocs(t *testing.T, dbname, colname string, writeConcern *writeconcern.WriteConcern, docs ...bsoncore.Document) {
	err := operation.NewInsert(docs...).Collection(colname).Database(dbname).
		Deployment(Topology(t)).ServerSelector(description.WriteSelector()).Execute(context.Background())
	require.NoError(t, err)
}

// EnableMaxTimeFailPoint turns on the max time fail point in the test cluster.
func EnableMaxTimeFailPoint(t *testing.T, s *topology.Server) error {
	cmd := bsoncore.BuildDocumentFromElements(nil,
		bsoncore.AppendStringElement(nil, "configureFailPoint", "maxTimeAlwaysTimeOut"),
		bsoncore.AppendStringElement(nil, "mode", "alwaysOn"),
	)
	return operation.NewCommand(cmd).
		Database("admin").Deployment(driver.SingleServerDeployment{Server: s}).
		Execute(context.Background())
}

// DisableMaxTimeFailPoint turns off the max time fail point in the test cluster.
func DisableMaxTimeFailPoint(t *testing.T, s *topology.Server) {
	cmd := bsoncore.BuildDocumentFromElements(nil,
		bsoncore.AppendStringElement(nil, "configureFailPoint", "maxTimeAlwaysTimeOut"),
		bsoncore.AppendStringElement(nil, "mode", "off"),
	)
	_ = operation.NewCommand(cmd).
		Database("admin").Deployment(driver.SingleServerDeployment{Server: s}).
		Execute(context.Background())
}

// RunCommand runs an arbitrary command on a given database of target server
func RunCommand(t *testing.T, s *topology.Server, db string, cmd bsoncore.Document) (bsoncore.Document, error) {
	op := operation.NewCommand(cmd).
		Database(db).Deployment(driver.SingleServerDeployment{Server: s})
	err := op.Execute(context.Background())
	res := op.Result()
	return res, err
}
