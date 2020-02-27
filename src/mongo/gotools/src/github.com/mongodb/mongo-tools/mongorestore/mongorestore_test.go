// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"testing"

	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/options"
	"github.com/mongodb/mongo-tools-common/testtype"
	"github.com/mongodb/mongo-tools-common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
)

const (
	mioSoeFile = "testdata/10k1dup10k.bson"
)

func init() {
	// bump up the verbosity to make checking debug log output possible
	log.SetVerbosity(&options.Verbosity{
		VLevel: 4,
	})
}

func getRestoreWithArgs(additionalArgs ...string) (*MongoRestore, error) {
	opts, err := ParseOptions(append(testutil.GetBareArgs(), additionalArgs...), "", "")
	if err != nil {
		return nil, fmt.Errorf("error parsing args: %v", err)
	}

	restore, err := New(opts)
	if err != nil {
		return nil, fmt.Errorf("error making new instance of mongorestore: %v", err)
	}

	return restore, nil
}

func TestMongorestore(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)
	_, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}

	Convey("With a test MongoRestore", t, func() {
		args := []string{
			NumParallelCollectionsOption, "1",
			NumInsertionWorkersOption, "1",
		}

		restore, err := getRestoreWithArgs(args...)
		So(err, ShouldBeNil)

		session, _ := restore.SessionProvider.GetSession()

		db := session.Database("db1")
		Convey("and majority is used as the default write concern", func() {
			So(db.WriteConcern(), ShouldResemble, writeconcern.New(writeconcern.WMajority()))
		})

		c1 := db.Collection("c1")
		c1.Drop(nil)
		Convey("and an explicit target restores from that dump directory", func() {
			restore.TargetDirectory = "testdata/testdirs"
			result := restore.Restore()
			So(result.Err, ShouldBeNil)
			So(result.Successes, ShouldEqual, 100)
			So(result.Failures, ShouldEqual, 0)
			count, err := c1.CountDocuments(nil, bson.M{})
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
			result := restore.Restore()
			So(result.Err, ShouldBeNil)
			count, err := c1.CountDocuments(nil, bson.M{})
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 100)
		})
	})
}

func TestMongorestoreCantPreserveUUID(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)
	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}
	fcv := testutil.GetFCV(session)
	if cmp, err := testutil.CompareFCV(fcv, "3.6"); err != nil || cmp >= 0 {
		t.Skip("Requires server with FCV less than 3.6")
	}

	Convey("PreserveUUID restore with incompatible destination FCV errors", func() {
		args := []string{
			NumParallelCollectionsOption, "1",
			NumInsertionWorkersOption, "1",
			PreserveUUIDOption,
			DropOption,
			"testdata/oplogdump",
		}
		restore, err := getRestoreWithArgs(args...)
		So(err, ShouldBeNil)

		result := restore.Restore()
		So(result.Err, ShouldNotBeNil)
		So(err.Error(), ShouldContainSubstring, "target host does not support --preserveUUID")
	})
}

func TestMongorestorePreserveUUID(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)
	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}
	fcv := testutil.GetFCV(session)
	if cmp, err := testutil.CompareFCV(fcv, "3.6"); err != nil || cmp < 0 {
		t.Skip("Requires server with FCV 3.6 or later")
	}

	// From mongorestore/testdata/oplogdump/db1/c1.metadata.json
	originalUUID := "699f503df64b4aa8a484a8052046fa3a"

	Convey("With a test MongoRestore", t, func() {
		c1 := session.Database("db1").Collection("c1")
		c1.Drop(nil)

		Convey("normal restore gives new UUID", func() {
			args := []string{
				NumParallelCollectionsOption, "1",
				NumInsertionWorkersOption, "1",
				"testdata/oplogdump",
			}
			restore, err := getRestoreWithArgs(args...)
			So(err, ShouldBeNil)

			result := restore.Restore()
			So(result.Err, ShouldBeNil)
			count, err := c1.CountDocuments(nil, bson.M{})
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 5)
			info, err := db.GetCollectionInfo(c1)
			So(err, ShouldBeNil)
			So(info.GetUUID(), ShouldNotEqual, originalUUID)
		})

		Convey("PreserveUUID restore without drop errors", func() {
			args := []string{
				NumParallelCollectionsOption, "1",
				NumInsertionWorkersOption, "1",
				PreserveUUIDOption,
				"testdata/oplogdump",
			}
			restore, err := getRestoreWithArgs(args...)
			So(err, ShouldBeNil)

			result := restore.Restore()
			So(result.Err, ShouldNotBeNil)
			So(result.Err.Error(), ShouldContainSubstring, "cannot specify --preserveUUID without --drop")
		})

		Convey("PreserveUUID with drop preserves UUID", func() {
			args := []string{
				NumParallelCollectionsOption, "1",
				NumInsertionWorkersOption, "1",
				PreserveUUIDOption,
				DropOption,
				"testdata/oplogdump",
			}
			restore, err := getRestoreWithArgs(args...)
			So(err, ShouldBeNil)

			result := restore.Restore()
			So(result.Err, ShouldBeNil)
			count, err := c1.CountDocuments(nil, bson.M{})
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 5)
			info, err := db.GetCollectionInfo(c1)
			So(err, ShouldBeNil)
			So(info.GetUUID(), ShouldEqual, originalUUID)
		})

		Convey("PreserveUUID on a file without UUID metadata errors", func() {
			args := []string{
				NumParallelCollectionsOption, "1",
				NumInsertionWorkersOption, "1",
				PreserveUUIDOption,
				DropOption,
				"testdata/testdirs",
			}
			restore, err := getRestoreWithArgs(args...)
			So(err, ShouldBeNil)

			result := restore.Restore()
			So(result.Err, ShouldNotBeNil)
			So(result.Err.Error(), ShouldContainSubstring, "--preserveUUID used but no UUID found")
		})

	})
}

// generateTestData creates the files used in TestMongorestoreMIOSOE
func generateTestData() error {
	// If file exists already, don't both regenerating it.
	if _, err := os.Stat(mioSoeFile); err == nil {
		return nil
	}

	f, err := os.Create(mioSoeFile)
	if err != nil {
		return err
	}
	w := bufio.NewWriter(f)

	// 10k unique _id's
	for i := 1; i < 10001; i++ {
		buf, err := bson.Marshal(bson.D{{"_id", i}})
		if err != nil {
			return err
		}
		_, err = w.Write(buf)
		if err != nil {
			return err
		}
	}

	// 1 duplicate _id
	buf, err := bson.Marshal(bson.D{{"_id", 5}})
	if err != nil {
		return err
	}
	_, err = w.Write(buf)
	if err != nil {
		return err
	}

	// 10k unique _id's
	for i := 10001; i < 20001; i++ {
		buf, err := bson.Marshal(bson.D{{"_id", i}})
		if err != nil {
			return err
		}
		_, err = w.Write(buf)
		if err != nil {
			return err
		}
	}
	if err := w.Flush(); err != nil {
		return err
	}

	return nil
}

// test --maintainInsertionOrder and --stopOnError behavior
func TestMongorestoreMIOSOE(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)

	if err := generateTestData(); err != nil {
		t.Fatalf("Couldn't generate test data %v", err)
	}

	client, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}
	database := client.Database("miodb")
	coll := database.Collection("mio")

	Convey("default restore ignores dup key errors", t, func() {
		restore, err := getRestoreWithArgs(mioSoeFile,
			CollectionOption, coll.Name(),
			DBOption, database.Name(),
			DropOption)
		So(err, ShouldBeNil)
		So(restore.OutputOptions.MaintainInsertionOrder, ShouldBeFalse)

		result := restore.Restore()
		So(result.Err, ShouldBeNil)
		So(result.Successes, ShouldEqual, 20000)
		So(result.Failures, ShouldEqual, 1)

		count, err := coll.CountDocuments(nil, bson.M{})
		So(err, ShouldBeNil)
		So(count, ShouldEqual, 20000)
	})

	Convey("--maintainInsertionOrder stops exactly on dup key errors", t, func() {
		restore, err := getRestoreWithArgs(mioSoeFile,
			CollectionOption, coll.Name(),
			DBOption, database.Name(),
			DropOption,
			MaintainInsertionOrderOption)
		So(err, ShouldBeNil)
		So(restore.OutputOptions.MaintainInsertionOrder, ShouldBeTrue)
		So(restore.OutputOptions.NumInsertionWorkers, ShouldEqual, 1)

		result := restore.Restore()
		So(result.Err, ShouldNotBeNil)
		So(result.Successes, ShouldEqual, 10000)
		So(result.Failures, ShouldEqual, 1)

		count, err := coll.CountDocuments(nil, bson.M{})
		So(err, ShouldBeNil)
		So(count, ShouldEqual, 10000)
	})

	Convey("--stopOnError stops on dup key errors", t, func() {
		restore, err := getRestoreWithArgs(mioSoeFile,
			CollectionOption, coll.Name(),
			DBOption, database.Name(),
			DropOption,
			StopOnErrorOption,
			NumParallelCollectionsOption, "1")
		So(err, ShouldBeNil)
		So(restore.OutputOptions.StopOnError, ShouldBeTrue)

		result := restore.Restore()
		So(result.Err, ShouldNotBeNil)
		So(result.Successes, ShouldAlmostEqual, 10000, restore.OutputOptions.BulkBufferSize)
		So(result.Failures, ShouldEqual, 1)

		count, err := coll.CountDocuments(nil, bson.M{})
		So(err, ShouldBeNil)
		So(count, ShouldAlmostEqual, 10000, restore.OutputOptions.BulkBufferSize)
	})

	_ = database.Drop(nil)
}

func TestDeprecatedIndexOptions(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)
	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}

	Convey("With a test MongoRestore", t, func() {
		args := []string{
			NumParallelCollectionsOption, "1",
			NumInsertionWorkersOption, "1",
		}

		restore, err := getRestoreWithArgs(args...)
		So(err, ShouldBeNil)

		session, _ = restore.SessionProvider.GetSession()

		db := session.Database("indextest")

		coll := db.Collection("test_collection")
		coll.Drop(nil)
		defer func() {
			coll.Drop(nil)
		}()
		Convey("Creating index with invalid option should throw error", func() {
			restore.TargetDirectory = "testdata/indextestdump"
			result := restore.Restore()
			So(result.Err, ShouldNotBeNil)
			So(result.Err.Error(), ShouldStartWith, `indextest.test_collection: error creating indexes for indextest.test_collection: createIndex error: (InvalidIndexSpecificationOption)`)

			So(result.Successes, ShouldEqual, 100)
			So(result.Failures, ShouldEqual, 0)
			count, err := coll.CountDocuments(nil, bson.M{})
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 100)
		})

		coll.Drop(nil)

		args = []string{
			NumParallelCollectionsOption, "1",
			NumInsertionWorkersOption, "1",
			ConvertLegacyIndexesOption, "true",
		}

		restore, err = getRestoreWithArgs(args...)
		So(err, ShouldBeNil)

		Convey("Creating index with invalid option and --ignoreInvalidIndexOptions should succeed", func() {
			restore.TargetDirectory = "testdata/indextestdump"
			result := restore.Restore()
			So(result.Err, ShouldBeNil)

			So(result.Successes, ShouldEqual, 100)
			So(result.Failures, ShouldEqual, 0)
			count, err := coll.CountDocuments(nil, bson.M{})
			So(err, ShouldBeNil)
			So(count, ShouldEqual, 100)
		})
	})
}

func TestRestoreUsersOrRoles(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)
	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}

	Convey("With a test MongoRestore", t, func() {
		args := []string{
			NumParallelCollectionsOption, "1",
			NumInsertionWorkersOption, "1",
		}

		restore, err := getRestoreWithArgs(args...)
		So(err, ShouldBeNil)

		session, _ = restore.SessionProvider.GetSession()

		db := session.Database("admin")

		Convey("Restoring users and roles should drop tempusers and temproles", func() {
			restore.TargetDirectory = "testdata/usersdump"
			result := restore.Restore()
			So(result.Err, ShouldBeNil)

			adminCollections, err := db.ListCollectionNames(context.Background(), bson.M{})
			So(err, ShouldBeNil)

			for _, collName := range adminCollections {
				So(collName, ShouldNotEqual, "tempusers")
				So(collName, ShouldNotEqual, "temproles")
			}
		})
	})
}

func TestKnownCollections(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)
	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}

	Convey("With a test MongoRestore", t, func() {
		args := []string{
			NumParallelCollectionsOption, "1",
			NumInsertionWorkersOption, "1",
		}

		restore, err := getRestoreWithArgs(args...)
		So(err, ShouldBeNil)

		session, _ = restore.SessionProvider.GetSession()
		db := session.Database("test")
		defer func() {
			db.Collection("foo").Drop(nil)
		}()

		Convey("Once collection foo has been restored, it should exist in restore.knownCollections", func() {
			restore.TargetDirectory = "testdata/foodump"
			result := restore.Restore()
			So(result.Err, ShouldBeNil)

			var namespaceExistsInCache bool
			if cols, ok := restore.knownCollections["test"]; ok {
				for _, collName := range cols {
					if collName == "foo" {
						namespaceExistsInCache = true
					}
				}
			}
			So(namespaceExistsInCache, ShouldBeTrue)
		})
	})
}

func TestFixHashedIndexes(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)
	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}

	type indexRes struct {
		Key bson.D
	}

	Convey("Test MongoRestore with hashed indexes and --fixHashedIndexes", t, func() {
		args := []string{
			FixDottedHashedIndexesOption,
		}

		restore, err := getRestoreWithArgs(args...)
		So(err, ShouldBeNil)

		session, _ = restore.SessionProvider.GetSession()
		db := session.Database("testdata")

		defer func() {
			db.Collection("hashedIndexes").Drop(nil)
		}()

		Convey("The index for a.b should be changed from 'hashed' to 1, since it is dotted", func() {
			restore.TargetDirectory = "testdata/hashedIndexes.bson"
			result := restore.Restore()
			So(result.Err, ShouldBeNil)

			indexes := db.Collection("hashedIndexes").Indexes()
			c, err := indexes.List(context.Background())
			So(err, ShouldBeNil)
			var res indexRes

			for c.Next(context.Background()) {
				err := c.Decode(&res)
				So(err, ShouldBeNil)
				for _, key := range res.Key {
					if key.Key == "b" {
						So(key.Value, ShouldEqual, "hashed")
					} else if key.Key == "a.a" {
						So(key.Value, ShouldEqual, 1)
					} else if key.Key == "a.b" {
						So(key.Value, ShouldEqual, 1)
					} else if key.Key != "_id" {
						t.Fatalf("Unexepected Index: %v", key.Key)
					}
				}
			}
		})
	})

	Convey("Test MongoRestore with hashed indexes without --fixHashedIndexes", t, func() {
		args := []string{}

		restore, err := getRestoreWithArgs(args...)
		So(err, ShouldBeNil)

		session, _ = restore.SessionProvider.GetSession()
		db := session.Database("testdata")

		defer func() {
			db.Collection("hashedIndexes").Drop(nil)
		}()

		Convey("All indexes should be unchanged", func() {
			restore.TargetDirectory = "testdata/hashedIndexes.bson"
			result := restore.Restore()
			So(result.Err, ShouldBeNil)

			indexes := db.Collection("hashedIndexes").Indexes()
			c, err := indexes.List(context.Background())
			So(err, ShouldBeNil)
			var res indexRes

			for c.Next(context.Background()) {
				err := c.Decode(&res)
				So(err, ShouldBeNil)
				for _, key := range res.Key {
					if key.Key == "b" {
						So(key.Value, ShouldEqual, "hashed")
					} else if key.Key == "a.a" {
						So(key.Value, ShouldEqual, 1)
					} else if key.Key == "a.b" {
						So(key.Value, ShouldEqual, "hashed")
					} else if key.Key != "_id" {
						t.Fatalf("Unexepected Index: %v", key.Key)
					}
				}
			}
		})
	})
}
