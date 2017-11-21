// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

import (
	"testing"

	"github.com/mongodb/mongo-tools/common/intents"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
)

const ExistsDB = "restore_collection_exists"

func TestCollectionExists(t *testing.T) {

	testutil.VerifyTestType(t, testutil.IntegrationTestType)
	_, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}

	Convey("With a test mongorestore", t, func() {
		sessionProvider, _, err := testutil.GetBareSessionProvider()
		So(err, ShouldBeNil)

		restore := &MongoRestore{
			SessionProvider: sessionProvider,
		}

		Convey("and some test data in a server", func() {
			session, err := restore.SessionProvider.GetSession()
			So(err, ShouldBeNil)
			So(session.DB(ExistsDB).C("one").Insert(bson.M{}), ShouldBeNil)
			So(session.DB(ExistsDB).C("two").Insert(bson.M{}), ShouldBeNil)
			So(session.DB(ExistsDB).C("three").Insert(bson.M{}), ShouldBeNil)

			Convey("collections that exist should return true", func() {
				exists, err := restore.CollectionExists(&intents.Intent{DB: ExistsDB, C: "one"})
				So(err, ShouldBeNil)
				So(exists, ShouldBeTrue)
				exists, err = restore.CollectionExists(&intents.Intent{DB: ExistsDB, C: "two"})
				So(err, ShouldBeNil)
				So(exists, ShouldBeTrue)
				exists, err = restore.CollectionExists(&intents.Intent{DB: ExistsDB, C: "three"})
				So(err, ShouldBeNil)
				So(exists, ShouldBeTrue)

				Convey("and those that do not exist should return false", func() {
					exists, err = restore.CollectionExists(&intents.Intent{DB: ExistsDB, C: "four"})
					So(err, ShouldBeNil)
					So(exists, ShouldBeFalse)
				})
			})

			Reset(func() {
				session.DB(ExistsDB).DropDatabase()
				session.Close()
			})
		})

		Convey("and a fake cache should be used instead of the server when it exists", func() {
			restore.knownCollections = map[string][]string{
				ExistsDB: []string{"cats", "dogs", "snakes"},
			}
			exists, err := restore.CollectionExists(&intents.Intent{DB: ExistsDB, C: "dogs"})
			So(err, ShouldBeNil)
			So(exists, ShouldBeTrue)
			exists, err = restore.CollectionExists(&intents.Intent{DB: ExistsDB, C: "two"})
			So(err, ShouldBeNil)
			So(exists, ShouldBeFalse)
		})
	})
}

func TestGetDumpAuthVersion(t *testing.T) {

	testutil.VerifyTestType(t, testutil.UnitTestType)
	restore := &MongoRestore{}

	Convey("With a test mongorestore", t, func() {
		Convey("and no --restoreDbUsersAndRoles", func() {
			restore = &MongoRestore{
				InputOptions: &InputOptions{},
				ToolOptions:  &commonOpts.ToolOptions{},
				NSOptions:    &NSOptions{},
			}
			Convey("auth version 1 should be detected", func() {
				restore.manager = intents.NewIntentManager()
				version, err := restore.GetDumpAuthVersion()
				So(err, ShouldBeNil)
				So(version, ShouldEqual, 1)
			})

			Convey("auth version 3 should be detected", func() {
				restore.manager = intents.NewIntentManager()
				intent := &intents.Intent{
					DB:       "admin",
					C:        "system.version",
					Location: "testdata/auth_version_3.bson",
				}
				intent.BSONFile = &realBSONFile{path: "testdata/auth_version_3.bson", intent: intent}
				restore.manager.Put(intent)
				version, err := restore.GetDumpAuthVersion()
				So(err, ShouldBeNil)
				So(version, ShouldEqual, 3)
			})

			Convey("auth version 5 should be detected", func() {
				restore.manager = intents.NewIntentManager()
				intent := &intents.Intent{
					DB:       "admin",
					C:        "system.version",
					Location: "testdata/auth_version_5.bson",
				}
				intent.BSONFile = &realBSONFile{path: "testdata/auth_version_5.bson", intent: intent}
				restore.manager.Put(intent)
				version, err := restore.GetDumpAuthVersion()
				So(err, ShouldBeNil)
				So(version, ShouldEqual, 5)
			})
		})

		Convey("using --restoreDbUsersAndRoles", func() {
			restore = &MongoRestore{
				InputOptions: &InputOptions{
					RestoreDBUsersAndRoles: true,
				},
				ToolOptions: &commonOpts.ToolOptions{},
				NSOptions: &NSOptions{
					DB: "TestDB",
				},
			}

			Convey("auth version 3 should be detected when no file exists", func() {
				restore.manager = intents.NewIntentManager()
				version, err := restore.GetDumpAuthVersion()
				So(err, ShouldBeNil)
				So(version, ShouldEqual, 3)
			})

			Convey("auth version 3 should be detected when a version 3 file exists", func() {
				restore.manager = intents.NewIntentManager()
				intent := &intents.Intent{
					DB:       "admin",
					C:        "system.version",
					Location: "testdata/auth_version_3.bson",
				}
				intent.BSONFile = &realBSONFile{path: "testdata/auth_version_3.bson", intent: intent}
				restore.manager.Put(intent)
				version, err := restore.GetDumpAuthVersion()
				So(err, ShouldBeNil)
				So(version, ShouldEqual, 3)
			})

			Convey("auth version 5 should be detected", func() {
				restore.manager = intents.NewIntentManager()
				intent := &intents.Intent{
					DB:       "admin",
					C:        "system.version",
					Location: "testdata/auth_version_5.bson",
				}
				intent.BSONFile = &realBSONFile{path: "testdata/auth_version_5.bson", intent: intent}
				restore.manager.Put(intent)
				version, err := restore.GetDumpAuthVersion()
				So(err, ShouldBeNil)
				So(version, ShouldEqual, 5)
			})
		})
	})

}
