// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoexport

import (
	"bytes"
	"context"
	"encoding/json"
	"io/ioutil"
	"os"
	"testing"

	"github.com/mongodb/mongo-tools-common/bsonutil"
	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/options"
	"github.com/mongodb/mongo-tools-common/testtype"
	"github.com/mongodb/mongo-tools-common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
)

var (
	// database with test data
	testDB             = "mongoexport_test_db"
	testCollectionName = "coll1"
)

func simpleMongoExportOpts() Options {
	var toolOptions *options.ToolOptions

	// get ToolOptions from URI or defaults
	if uri := os.Getenv("MONGOD"); uri != "" {
		fakeArgs := []string{"--uri=" + uri}
		toolOptions = options.New("mongoexport", "", "", "", options.EnabledOptions{URI: true})
		toolOptions.URI.AddKnownURIParameters(options.KnownURIOptionsReadPreference)
		_, err := toolOptions.ParseArgs(fakeArgs)
		if err != nil {
			panic("Could not parse MONGOD environment variable")
		}
	} else {
		ssl := testutil.GetSSLOptions()
		auth := testutil.GetAuthOptions()
		connection := &options.Connection{
			Host: "localhost",
			Port: db.DefaultTestPort,
		}
		toolOptions = &options.ToolOptions{
			SSL:        &ssl,
			Connection: connection,
			Auth:       &auth,
			Verbosity:  &options.Verbosity{},
			URI:        &options.URI{},
		}
	}

	// Limit ToolOptions to test database
	toolOptions.Namespace = &options.Namespace{DB: testDB, Collection: testCollectionName}

	opts := Options{
		ToolOptions: toolOptions,
		OutputFormatOptions: &OutputFormatOptions{
			Type:       "json",
			JSONFormat: "canonical",
		},
		InputOptions: &InputOptions{},
	}

	log.SetVerbosity(toolOptions.Verbosity)
	return opts
}

func TestExtendedJSON(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.UnitTestType)

	Convey("Serializing a doc to extended JSON should work", t, func() {
		x := bson.M{
			"_id": primitive.NewObjectID(),
			"hey": "sup",
			"subdoc": bson.M{
				"subid": primitive.NewObjectID(),
			},
			"array": []interface{}{
				primitive.NewObjectID(),
				primitive.Undefined{},
			},
		}
		out, err := bsonutil.ConvertBSONValueToLegacyExtJSON(x)
		So(err, ShouldBeNil)

		jsonEncoder := json.NewEncoder(os.Stdout)
		jsonEncoder.Encode(out)
	})
}

func TestFieldSelect(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.UnitTestType)

	Convey("Using makeFieldSelector should return correct projection doc", t, func() {
		So(makeFieldSelector("a,b"), ShouldResemble, bson.M{"_id": 1, "a": 1, "b": 1})
		So(makeFieldSelector(""), ShouldResemble, bson.M{"_id": 1})
		So(makeFieldSelector("x,foo.baz"), ShouldResemble, bson.M{"_id": 1, "foo": 1, "x": 1})
	})
}

// Test exporting a collection with autoIndexId:false.  As of MongoDB 4.0,
// this is only allowed on the 'local' database.
func TestMongoExportTOOLS2174(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)
	log.SetWriter(ioutil.Discard)

	sessionProvider, _, err := testutil.GetBareSessionProvider()
	if err != nil {
		t.Fatalf("No cluster available: %v", err)
	}

	collName := "tools-2174"
	dbName := "local"

	var r1 bson.M
	sessionProvider.Run(bson.D{{"drop", collName}}, &r1, dbName)

	createCmd := bson.D{
		{"create", collName},
		{"autoIndexId", false},
	}
	var r2 bson.M
	err = sessionProvider.Run(createCmd, &r2, dbName)
	if err != nil {
		t.Fatalf("Error creating capped, no-autoIndexId collection: %v", err)
	}

	Convey("testing dumping a capped, autoIndexId:false collection", t, func() {
		opts := simpleMongoExportOpts()
		opts.Collection = collName
		opts.DB = dbName

		me, err := New(opts)
		So(err, ShouldBeNil)
		defer me.Close()
		out := &bytes.Buffer{}
		_, err = me.Export(out)
		So(err, ShouldBeNil)
	})
}

// Test exporting a collection, _id should only be hinted iff
// this is not a wired tiger collection.
func TestMongoExportTOOLS1952(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)
	log.SetWriter(ioutil.Discard)

	sessionProvider, _, err := testutil.GetBareSessionProvider()
	if err != nil {
		t.Fatalf("No cluster available: %v", err)
	}

	session, err := sessionProvider.GetSession()
	if err != nil {
		t.Fatalf("Failed to get session: %v", err)
	}

	collName := "tools-1952-export"
	dbName := "test"
	ns := dbName + "." + collName

	dbStruct := session.Database(dbName)

	var r1 bson.M
	sessionProvider.Run(bson.D{{"drop", collName}}, &r1, dbName)

	createCmd := bson.D{
		{"create", collName},
	}

	var r2 bson.M
	err = sessionProvider.Run(createCmd, &r2, dbName)
	if err != nil {
		t.Fatalf("Error creating collection: %v", err)
	}

	// Check whether we are using MMAPV1.
	isMMAPV1, err := db.IsMMAPV1(dbStruct, collName)
	if err != nil {
		t.Fatalf("Failed to determine storage engine %v", err)
	}

	// Turn on profiling.
	profileCmd := bson.D{
		{"profile", 2},
	}

	err = sessionProvider.Run(profileCmd, &r2, dbName)
	if err != nil {
		t.Fatalf("Failed to turn on profiling: %v", err)
	}

	profileCollection := dbStruct.Collection("system.profile")

	Convey("testing exporting a collection", t, func() {
		opts := simpleMongoExportOpts()
		opts.Collection = collName
		opts.DB = dbName

		me, err := New(opts)
		So(err, ShouldBeNil)
		defer me.Close()
		out := &bytes.Buffer{}
		_, err = me.Export(out)
		So(err, ShouldBeNil)

		// If we are using mmapv1, we should be hinting an index or using a
		// snapshot, depending on the version.
		count, err := profileCollection.CountDocuments(context.Background(),
			bson.D{
				{"ns", ns},
				{"op", "query"},
				{"$or", []interface{}{
					// 4.0+
					bson.D{{"command.hint._id", 1}},
					// 3.6
					bson.D{{"command.$nsapshot", true}},
					bson.D{{"command.snapshot", true}},
					// 3.4 and previous
					bson.D{{"query.$snapshot", true}},
					bson.D{{"query.snapshot", true}},
					bson.D{{"query.hint._id", 1}},
				}},
			},
		)
		So(err, ShouldBeNil)
		if isMMAPV1 {
			// There should be exactly one query that matches in MMAPV1
			So(count, ShouldEqual, 1)
		} else {
			// In modern storage engines, there should be no hints, so there
			// should be 0 matches.
			So(count, ShouldEqual, 0)
		}
	})
}
