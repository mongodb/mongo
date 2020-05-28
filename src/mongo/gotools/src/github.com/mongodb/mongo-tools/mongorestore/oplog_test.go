// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

import (
	"context"
	"io/ioutil"
	"os"
	"strings"
	"testing"

	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools-common/testtype"
	"github.com/mongodb/mongo-tools-common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
)

func TestTimestampStringParsing(t *testing.T) {

	testtype.SkipUnlessTestType(t, testtype.UnitTestType)

	Convey("Testing some possible timestamp strings:", t, func() {
		Convey("123:456 [should pass]", func() {
			ts, err := ParseTimestampFlag("123:456")
			So(err, ShouldBeNil)
			So(ts, ShouldResemble, primitive.Timestamp{T: 123, I: 456})
		})

		Convey("123 [should pass]", func() {
			ts, err := ParseTimestampFlag("123")
			So(err, ShouldBeNil)
			So(ts, ShouldResemble, primitive.Timestamp{T: 123, I: 0})
		})

		Convey("123: [should pass]", func() {
			ts, err := ParseTimestampFlag("123:")
			So(err, ShouldBeNil)
			So(ts, ShouldResemble, primitive.Timestamp{T: 123, I: 0})
		})

		Convey("123.123 [should fail]", func() {
			ts, err := ParseTimestampFlag("123.123")
			So(err, ShouldNotBeNil)
			So(ts, ShouldResemble, primitive.Timestamp{})
		})

		Convey(": [should fail]", func() {
			ts, err := ParseTimestampFlag(":")
			So(err, ShouldNotBeNil)
			So(ts, ShouldResemble, primitive.Timestamp{})
		})

		Convey("1:1:1 [should fail]", func() {
			ts, err := ParseTimestampFlag("1:1:1")
			So(err, ShouldNotBeNil)
			So(ts, ShouldResemble, primitive.Timestamp{})
		})

		Convey("cats [should fail]", func() {
			ts, err := ParseTimestampFlag("cats")
			So(err, ShouldNotBeNil)
			So(ts, ShouldResemble, primitive.Timestamp{})
		})

		Convey("[empty string] [should fail]", func() {
			ts, err := ParseTimestampFlag("")
			So(err, ShouldNotBeNil)
			So(ts, ShouldResemble, primitive.Timestamp{})
		})
	})
}

func TestValidOplogLimitChecking(t *testing.T) {

	testtype.SkipUnlessTestType(t, testtype.UnitTestType)

	Convey("With a MongoRestore instance with oplogLimit of 5:0", t, func() {
		mr := &MongoRestore{
			oplogLimit: primitive.Timestamp{T: 5, I: 0},
		}

		Convey("an oplog entry with ts=1000:0 should be invalid", func() {
			So(mr.TimestampBeforeLimit(primitive.Timestamp{T: 1000, I: 0}), ShouldBeFalse)
		})

		Convey("an oplog entry with ts=5:1 should be invalid", func() {
			So(mr.TimestampBeforeLimit(primitive.Timestamp{T: 5, I: 1}), ShouldBeFalse)
		})

		Convey("an oplog entry with ts=5:0 should be invalid", func() {
			So(mr.TimestampBeforeLimit(primitive.Timestamp{T: 5, I: 0}), ShouldBeFalse)
		})

		Convey("an oplog entry with ts=4:9 should be valid", func() {
			So(mr.TimestampBeforeLimit(primitive.Timestamp{T: 4, I: 9}), ShouldBeTrue)
		})

		Convey("an oplog entry with ts=4:0 should be valid", func() {
			So(mr.TimestampBeforeLimit(primitive.Timestamp{T: 4, I: 0}), ShouldBeTrue)
		})

		Convey("an oplog entry with ts=0:1 should be valid", func() {
			So(mr.TimestampBeforeLimit(primitive.Timestamp{T: 0, I: 1}), ShouldBeTrue)
		})
	})

	Convey("With a MongoRestore instance with no oplogLimit", t, func() {
		mr := &MongoRestore{}

		Convey("an oplog entry with ts=1000:0 should be valid", func() {
			So(mr.TimestampBeforeLimit(primitive.Timestamp{T: 1000, I: 0}), ShouldBeTrue)
		})

		Convey("an oplog entry with ts=5:1 should be valid", func() {
			So(mr.TimestampBeforeLimit(primitive.Timestamp{T: 5, I: 1}), ShouldBeTrue)
		})

		Convey("an oplog entry with ts=5:0 should be valid", func() {
			So(mr.TimestampBeforeLimit(primitive.Timestamp{T: 5, I: 0}), ShouldBeTrue)
		})
	})

}

func TestOplogRestore(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)

	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}
	fcv := testutil.GetFCV(session)
	var shouldPreserveUUID bool
	if cmp, err := testutil.CompareFCV(fcv, "3.6"); err != nil || cmp >= 0 {
		shouldPreserveUUID = true
	}

	Convey("With a test MongoRestore", t, func() {
		args := []string{
			DirectoryOption, "testdata/oplogdump",
			OplogReplayOption,
			NumParallelCollectionsOption, "1",
			NumInsertionWorkersOption, "1",
			DropOption,
		}
		if shouldPreserveUUID {
			args = append(args, PreserveUUIDOption)
		}

		restore, err := getRestoreWithArgs(args...)
		So(err, ShouldBeNil)
		c1 := session.Database("db1").Collection("c1")
		c1.Drop(nil)

		// Run mongorestore
		result := restore.Restore()
		So(result.Err, ShouldBeNil)
		So(result.Failures, ShouldEqual, 0)

		// Verify restoration
		count, err := c1.CountDocuments(nil, bson.M{})
		So(err, ShouldBeNil)
		So(count, ShouldEqual, 10)
		session.Disconnect(context.Background())
	})
}

func TestOplogRestoreMaxDocumentSize(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)

	session, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}
	fcv := testutil.GetFCV(session)
	var shouldPreserveUUID bool
	if cmp, err := testutil.CompareFCV(fcv, "3.6"); err != nil || cmp >= 0 {
		shouldPreserveUUID = true
	}

	c1 := session.Database("db1").Collection("c1")
	c1.Drop(nil)

	Convey("With a test MongoRestore replay oplog with a 16 MiB document", t, func() {
		// Generate an oplog document and verify that size exceeds 16 MiB.
		oplogBytes, err := generateOplogWith16MiBDocument()
		So(err, ShouldBeNil)
		So(len(oplogBytes), ShouldBeGreaterThan, db.MaxBSONSize)

		// Temporarily write the oplog document to testdata/oplogdumpmaxsize/oplog.bson
		err = ioutil.WriteFile("testdata/oplogdumpmaxsize/oplog.bson", oplogBytes, 0644)
		So(err, ShouldBeNil)
		defer os.Remove("testdata/oplogdumpmaxsize/oplog.bson")

		args := []string{
			DirectoryOption, "testdata/oplogdumpmaxsize",
			OplogReplayOption,
			NumParallelCollectionsOption, "1",
			NumInsertionWorkersOption, "1",
			DropOption,
		}
		if shouldPreserveUUID {
			args = append(args, PreserveUUIDOption)
		}

		restore, err := getRestoreWithArgs(args...)
		So(err, ShouldBeNil)

		// Make sure to drop the 16 MiB collection before disconnecting.
		defer session.Disconnect(context.Background())
		defer c1.Drop(nil)

		// Run mongorestore.
		result := restore.Restore()
		So(result.Err, ShouldBeNil)
		So(result.Failures, ShouldEqual, 0)

		// Verify restoration (5 docs in c1.bson + 1 doc in oplog.bson).
		count, err := c1.CountDocuments(nil, bson.M{})
		So(err, ShouldBeNil)
		So(count, ShouldEqual, 6)
	})
}

// Generates an oplog document that is greater than 16 MiB but less than 16 MiB + 16 KiB.
// Returns the oplog document's raw bytes.
func generateOplogWith16MiBDocument() ([]byte, error) {

	// Generate a document of the form {_id: X, key: Y} where the total document size
	// is equal to 16 MiB. Generates a long string for Y in order to reach 16 MiB.
	//
	// Here's a breakdown of bytes in the document:
	//
	// 4 bytes = document length
	// 1 byte = element type (ObjectID = \x07)
	// 4 bytes = key name ("_id" + \x00)
	// 12 bytes = ObjectID value
	// 1 byte = element type (string = \x02)
	// 4 bytes = key name ("key" + \x00)
	// 4 bytes = string length
	// X bytes = string of length X bytes
	// 1 byte = \x00
	// 1 byte = \x00
	//
	// Therefore the string length should be: 1024*1024*16 - 32

	size := 1024*1024*16 - 32

	idx, rawdoc := bsoncore.AppendDocumentStart(nil)
	rawdoc = bsoncore.AppendObjectIDElement(rawdoc, "_id", primitive.NewObjectID())
	rawdoc = bsoncore.AppendStringElement(rawdoc, "key", strings.Repeat("A", size))
	rawdoc, _ = bsoncore.AppendDocumentEnd(rawdoc, idx)

	// Creating the oplog document with the above 16 MiB document will allow
	// the oplog document to exceed 16 MiB with the additional metadata.
	var doc bson.D
	bson.Unmarshal(rawdoc, &doc)
	oplog := db.Oplog{
		Version:   2,
		Operation: "i",
		Namespace: "db1.c1",
		Object:    doc,
	}

	return bson.Marshal(oplog)
}

func TestOplogRestoreTools2002(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)
	_, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}

	Convey("With a test MongoRestore", t, func() {
		args := []string{
			DirectoryOption, "testdata/tools-2002",
			OplogReplayOption,
			NumParallelCollectionsOption, "1",
			NumInsertionWorkersOption, "1",
			DropOption,
		}
		restore, err := getRestoreWithArgs(args...)
		So(err, ShouldBeNil)

		// Run mongorestore
		result := restore.Restore()
		So(result.Err, ShouldBeNil)
		So(result.Failures, ShouldEqual, 0)
	})
}
