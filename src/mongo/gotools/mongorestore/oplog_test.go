package mongorestore

import (
	"testing"

	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
)

func TestTimestampStringParsing(t *testing.T) {

	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Testing some possible timestamp strings:", t, func() {
		Convey("123:456 [should pass]", func() {
			ts, err := ParseTimestampFlag("123:456")
			So(err, ShouldBeNil)
			So(ts, ShouldEqual, (int64(123)<<32 | int64(456)))
		})

		Convey("123 [should pass]", func() {
			ts, err := ParseTimestampFlag("123")
			So(err, ShouldBeNil)
			So(ts, ShouldEqual, int64(123)<<32)
		})

		Convey("123: [should pass]", func() {
			ts, err := ParseTimestampFlag("123:")
			So(err, ShouldBeNil)
			So(ts, ShouldEqual, int64(123)<<32)
		})

		Convey("123.123 [should fail]", func() {
			ts, err := ParseTimestampFlag("123.123")
			So(err, ShouldNotBeNil)
			So(ts, ShouldEqual, 0)
		})

		Convey(": [should fail]", func() {
			ts, err := ParseTimestampFlag(":")
			So(err, ShouldNotBeNil)
			So(ts, ShouldEqual, 0)
		})

		Convey("1:1:1 [should fail]", func() {
			ts, err := ParseTimestampFlag("1:1:1")
			So(err, ShouldNotBeNil)
			So(ts, ShouldEqual, 0)
		})

		Convey("cats [should fail]", func() {
			ts, err := ParseTimestampFlag("cats")
			So(err, ShouldNotBeNil)
			So(ts, ShouldEqual, 0)
		})

		Convey("[empty string] [should fail]", func() {
			ts, err := ParseTimestampFlag("")
			So(err, ShouldNotBeNil)
			So(ts, ShouldEqual, 0)
		})
	})
}

func TestValidOplogLimitChecking(t *testing.T) {

	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With a MongoRestore instance with oplogLimit of 5:0", t, func() {
		mr := &MongoRestore{
			oplogLimit: bson.MongoTimestamp(int64(5) << 32),
		}

		Convey("an oplog entry with ts=1000:0 should be invalid", func() {
			So(mr.TimestampBeforeLimit(bson.MongoTimestamp(int64(1000)<<32)), ShouldBeFalse)
		})

		Convey("an oplog entry with ts=5:1 should be invalid", func() {
			So(mr.TimestampBeforeLimit(bson.MongoTimestamp(int64(5)<<32|1)), ShouldBeFalse)
		})

		Convey("an oplog entry with ts=5:0 should be invalid", func() {
			So(mr.TimestampBeforeLimit(bson.MongoTimestamp(int64(5)<<32)), ShouldBeFalse)
		})

		Convey("an oplog entry with ts=4:9 should be valid", func() {
			So(mr.TimestampBeforeLimit(bson.MongoTimestamp(int64(4)<<32|9)), ShouldBeTrue)
		})

		Convey("an oplog entry with ts=4:0 should be valid", func() {
			So(mr.TimestampBeforeLimit(bson.MongoTimestamp(int64(4)<<32)), ShouldBeTrue)
		})

		Convey("an oplog entry with ts=0:1 should be valid", func() {
			So(mr.TimestampBeforeLimit(bson.MongoTimestamp(1)), ShouldBeTrue)
		})
	})

	Convey("With a MongoRestore instance with no oplogLimit", t, func() {
		mr := &MongoRestore{}

		Convey("an oplog entry with ts=1000:0 should be valid", func() {
			So(mr.TimestampBeforeLimit(bson.MongoTimestamp(int64(1000)<<32)), ShouldBeTrue)
		})

		Convey("an oplog entry with ts=5:1 should be valid", func() {
			So(mr.TimestampBeforeLimit(bson.MongoTimestamp(int64(5)<<32|1)), ShouldBeTrue)
		})

		Convey("an oplog entry with ts=5:0 should be valid", func() {
			So(mr.TimestampBeforeLimit(bson.MongoTimestamp(int64(5)<<32)), ShouldBeTrue)
		})
	})

}
