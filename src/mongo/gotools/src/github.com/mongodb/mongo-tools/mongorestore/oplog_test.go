// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

import (
	"context"
	"testing"

	"github.com/mongodb/mongo-tools-common/testtype"
	"github.com/mongodb/mongo-tools-common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/primitive"
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
		err = restore.Restore()
		So(err, ShouldBeNil)

		// Verify restoration
		count, err := c1.CountDocuments(nil, bson.M{})
		So(err, ShouldBeNil)
		So(count, ShouldEqual, 10)
		session.Disconnect(context.Background())
	})
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
		err = restore.Restore()
		So(err, ShouldBeNil)
	})
}
