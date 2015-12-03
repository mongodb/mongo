package db

import (
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

func TestBufferedBulkInserterInserts(t *testing.T) {
	var bufBulk *BufferedBulkInserter

	testutil.VerifyTestType(t, "db")

	Convey("With a valid session", t, func() {
		opts := options.ToolOptions{
			Connection: &options.Connection{
				Port: DefaultTestPort,
			},
			SSL:  &options.SSL{},
			Auth: &options.Auth{},
		}
		provider, err := NewSessionProvider(opts)
		session, err := provider.GetSession()
		So(session, ShouldNotBeNil)
		So(err, ShouldBeNil)

		Convey("using a test collection and a doc limit of 3", func() {
			testCol := session.DB("tools-test").C("bulk1")
			bufBulk = NewBufferedBulkInserter(testCol, 3, false)
			So(bufBulk, ShouldNotBeNil)

			Convey("inserting 10 documents into the BufferedBulkInserter", func() {
				flushCount := 0
				for i := 0; i < 10; i++ {
					So(bufBulk.Insert(bson.D{}), ShouldBeNil)
					if bufBulk.docCount%3 == 0 {
						flushCount++
					}
				}

				Convey("should have flushed 3 times with one doc still buffered", func() {
					So(flushCount, ShouldEqual, 3)
					So(bufBulk.byteCount, ShouldBeGreaterThan, 0)
					So(bufBulk.docCount, ShouldEqual, 1)
				})
			})
		})

		Convey("using a test collection and a doc limit of 1", func() {
			testCol := session.DB("tools-test").C("bulk2")
			bufBulk = NewBufferedBulkInserter(testCol, 1, false)
			So(bufBulk, ShouldNotBeNil)

			Convey("inserting 10 documents into the BufferedBulkInserter and flushing", func() {
				for i := 0; i < 10; i++ {
					So(bufBulk.Insert(bson.D{}), ShouldBeNil)
				}
				So(bufBulk.Flush(), ShouldBeNil)

				Convey("should have no docs buffered", func() {
					So(bufBulk.docCount, ShouldEqual, 0)
					So(bufBulk.byteCount, ShouldEqual, 0)
				})
			})
		})

		Convey("using a test collection and a doc limit of 1000", func() {
			testCol := session.DB("tools-test").C("bulk3")
			bufBulk = NewBufferedBulkInserter(testCol, 100, false)
			So(bufBulk, ShouldNotBeNil)

			Convey("inserting 1,000,000 documents into the BufferedBulkInserter and flushing", func() {
				session.SetSocketTimeout(0)

				for i := 0; i < 1000000; i++ {
					bufBulk.Insert(bson.M{"_id": i})
				}
				So(bufBulk.Flush(), ShouldBeNil)

				Convey("should have inserted all of the documents", func() {
					count, err := testCol.Count()
					So(err, ShouldBeNil)
					So(count, ShouldEqual, 1000000)

					// test values
					testDoc := bson.M{}
					err = testCol.Find(bson.M{"_id": 477232}).One(&testDoc)
					So(err, ShouldBeNil)
					So(testDoc["_id"], ShouldEqual, 477232)
					err = testCol.Find(bson.M{"_id": 999999}).One(&testDoc)
					So(err, ShouldBeNil)
					So(testDoc["_id"], ShouldEqual, 999999)
					err = testCol.Find(bson.M{"_id": 1}).One(&testDoc)
					So(err, ShouldBeNil)
					So(testDoc["_id"], ShouldEqual, 1)

				})
			})
		})

		Reset(func() {
			session.DB("tools-test").DropDatabase()
		})
	})

}
