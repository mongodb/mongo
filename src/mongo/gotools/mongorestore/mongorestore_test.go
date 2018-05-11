// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	"github.com/mongodb/mongo-tools/common/util"

	"os"
	"testing"

	. "github.com/smartystreets/goconvey/convey"
)

func init() {
	// bump up the verbosity to make checking debug log output possible
	log.SetVerbosity(&options.Verbosity{
		VLevel: 4,
	})
}

var (
	testServer = "localhost"
	testPort   = db.DefaultTestPort
)

func TestMongorestore(t *testing.T) {
	testutil.VerifyTestType(t, testutil.IntegrationTestType)
	_, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}

	Convey("With a test MongoRestore", t, func() {
		inputOptions := &InputOptions{}
		outputOptions := &OutputOptions{
			NumParallelCollections: 1,
			NumInsertionWorkers:    1,
		}
		nsOptions := &NSOptions{}
		provider, toolOpts, err := testutil.GetBareSessionProvider()
		if err != nil {
			log.Logvf(log.Always, "error connecting to host: %v", err)
			os.Exit(util.ExitError)
		}
		restore := MongoRestore{
			ToolOptions:     toolOpts,
			OutputOptions:   outputOptions,
			InputOptions:    inputOptions,
			NSOptions:       nsOptions,
			SessionProvider: provider,
		}
		session, _ := provider.GetSession()
		defer session.Close()
		c1 := session.DB("db1").C("c1")
		c1.DropCollection()
		Convey("and an explicit target restores from that dump directory", func() {
			restore.TargetDirectory = "testdata/testdirs"
			err = restore.Restore()
			So(err, ShouldBeNil)
			count, err := c1.Count()
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 100)
		})

		Convey("and an target of '-' restores from standard input", func() {
			bsonFile, err := os.Open("testdata/testdirs/db1/c1.bson")
			restore.NSOptions.Collection = "c1"
			restore.NSOptions.DB = "db1"
			So(err, ShouldBeNil)
			restore.InputReader = bsonFile
			restore.TargetDirectory = "-"
			err = restore.Restore()
			So(err, ShouldBeNil)
			count, err := c1.Count()
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 100)
		})

	})
}

func TestMongorestoreCantPreserveUUID(t *testing.T) {
	testutil.VerifyTestType(t, testutil.IntegrationTestType)
	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}
	defer session.Close()
	fcv := testutil.GetFCV(session)
	if cmp, err := testutil.CompareFCV(fcv, "3.6"); err != nil || cmp >= 0 {
		t.Skip("Requires server with FCV less than 3.6")
	}

	Convey("With a test MongoRestore", t, func() {
		nsOptions := &NSOptions{}
		provider, toolOpts, err := testutil.GetBareSessionProvider()
		if err != nil {
			log.Logvf(log.Always, "error connecting to host: %v", err)
			os.Exit(util.ExitError)
		}

		Convey("PreserveUUID restore with incompatible destination FCV errors", func() {
			inputOptions := &InputOptions{}
			outputOptions := &OutputOptions{
				NumParallelCollections: 1,
				NumInsertionWorkers:    1,
				PreserveUUID:           true,
				Drop:                   true,
			}
			restore := MongoRestore{
				ToolOptions:     toolOpts,
				OutputOptions:   outputOptions,
				InputOptions:    inputOptions,
				NSOptions:       nsOptions,
				SessionProvider: provider,
			}
			restore.TargetDirectory = "testdata/oplogdump"
			err = restore.Restore()
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldContainSubstring, "target host does not support --preserveUUID")
		})
	})
}

func TestMongorestorePreserveUUID(t *testing.T) {
	testutil.VerifyTestType(t, testutil.IntegrationTestType)
	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}
	defer session.Close()
	fcv := testutil.GetFCV(session)
	if cmp, err := testutil.CompareFCV(fcv, "3.6"); err != nil || cmp < 0 {
		t.Skip("Requires server with FCV 3.6 or later")
	}

	// From mongorestore/testdata/oplogdump/db1/c1.metadata.json
	originalUUID := "699f503df64b4aa8a484a8052046fa3a"

	Convey("With a test MongoRestore", t, func() {
		nsOptions := &NSOptions{}
		provider, toolOpts, err := testutil.GetBareSessionProvider()
		if err != nil {
			log.Logvf(log.Always, "error connecting to host: %v", err)
			os.Exit(util.ExitError)
		}

		c1 := session.DB("db1").C("c1")
		c1.DropCollection()

		Convey("normal restore gives new UUID", func() {
			inputOptions := &InputOptions{}
			outputOptions := &OutputOptions{
				NumParallelCollections: 1,
				NumInsertionWorkers:    1,
			}
			restore := MongoRestore{
				ToolOptions:     toolOpts,
				OutputOptions:   outputOptions,
				InputOptions:    inputOptions,
				NSOptions:       nsOptions,
				SessionProvider: provider,
			}
			restore.TargetDirectory = "testdata/oplogdump"
			err = restore.Restore()
			So(err, ShouldBeNil)
			count, err := c1.Count()
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 5)
			info, err := db.GetCollectionInfo(c1)
			So(err, ShouldBeNil)
			So(info.GetUUID(), ShouldNotEqual, originalUUID)
		})

		Convey("PreserveUUID restore without drop errors", func() {
			inputOptions := &InputOptions{}
			outputOptions := &OutputOptions{
				NumParallelCollections: 1,
				NumInsertionWorkers:    1,
				PreserveUUID:           true,
			}
			restore := MongoRestore{
				ToolOptions:     toolOpts,
				OutputOptions:   outputOptions,
				InputOptions:    inputOptions,
				NSOptions:       nsOptions,
				SessionProvider: provider,
			}
			restore.TargetDirectory = "testdata/oplogdump"
			err = restore.Restore()
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldContainSubstring, "cannot specify --preserveUUID without --drop")
		})

		Convey("PreserveUUID with drop preserves UUID", func() {
			inputOptions := &InputOptions{}
			outputOptions := &OutputOptions{
				NumParallelCollections: 1,
				NumInsertionWorkers:    1,
				PreserveUUID:           true,
				Drop:                   true,
			}
			restore := MongoRestore{
				ToolOptions:     toolOpts,
				OutputOptions:   outputOptions,
				InputOptions:    inputOptions,
				NSOptions:       nsOptions,
				SessionProvider: provider,
			}
			restore.TargetDirectory = "testdata/oplogdump"
			err = restore.Restore()
			So(err, ShouldBeNil)
			count, err := c1.Count()
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 5)
			info, err := db.GetCollectionInfo(c1)
			So(err, ShouldBeNil)
			So(info.GetUUID(), ShouldEqual, originalUUID)
		})

		Convey("PreserveUUID on a file without UUID metadata errors", func() {
			inputOptions := &InputOptions{}
			outputOptions := &OutputOptions{
				NumParallelCollections: 1,
				NumInsertionWorkers:    1,
				PreserveUUID:           true,
				Drop:                   true,
			}
			restore := MongoRestore{
				ToolOptions:     toolOpts,
				OutputOptions:   outputOptions,
				InputOptions:    inputOptions,
				NSOptions:       nsOptions,
				SessionProvider: provider,
			}
			restore.TargetDirectory = "testdata/testdirs"
			err = restore.Restore()
			So(err, ShouldNotBeNil)
			So(err.Error(), ShouldContainSubstring, "--preserveUUID used but no UUID found")
		})

	})
}
