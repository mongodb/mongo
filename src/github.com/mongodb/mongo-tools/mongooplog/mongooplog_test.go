package mongooplog

import (
	"github.com/mongodb/mongo-tools/common/db"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongooplog/options"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

func TestBasicOps(t *testing.T) {
	var opts *commonopts.ToolOptions
	var sourceOpts *options.SourceOptions

	Convey("When replicating operations", t, func() {

		// specify localhost:27017 as the destination host
		opts = &commonopts.ToolOptions{
			Namespace: &commonopts.Namespace{},
			SSL:       &commonopts.SSL{},
			Auth:      &commonopts.Auth{},
			Kerberos:  &commonopts.Kerberos{},
			Connection: &commonopts.Connection{
				Host: "localhost",
				Port: "27017",
			},
		}

		// specify localhost:27017 as the source host
		sourceOpts = &options.SourceOptions{
			Seconds: 84600,            // the default
			OplogNS: "local.oplog.rs", // the default
		}

		Convey("all operations should be applied correctly, without"+
			" error", func() {

			// initialize a session provider for the source
			sourceSP, err := db.InitSessionProvider(*opts)
			So(err, ShouldBeNil)

			// initialize a session provider for the destination
			destSP, err := db.InitSessionProvider(*opts)
			So(err, ShouldBeNil)

			// clear out the oplog
			sess, err := sourceSP.GetSession()
			So(err, ShouldBeNil)
			defer sess.Close()
			oplogColl := sess.DB("local").C("oplog.rs")
			_, err = oplogColl.RemoveAll(bson.M{})
			So(err, ShouldBeNil)

			// clear out the collection we'll use for testing
			testColl := sess.DB("mongooplog_test").C("data")
			_, err = testColl.RemoveAll(bson.M{})
			So(err, ShouldBeNil)

			// insert some "ops" into the oplog to be found and applied
			op1 := &Oplog{
				Timestamp: bson.MongoTimestamp(1<<63 - 1), // years in the future
				HistoryID: 100,
				Version:   2,
				Operation: "i",
				Namespace: "mongooplog_test.data",
				Object: bson.M{
					"_id": 1,
				},
			}
			So(oplogColl.Insert(op1), ShouldBeNil)
			op2 := &Oplog{
				Timestamp: bson.MongoTimestamp(1<<63 - 1), // years in the future
				HistoryID: 200,
				Version:   2,
				Operation: "i",
				Namespace: "mongooplog_test.data",
				Object: bson.M{
					"_id": 2,
				},
			}
			So(oplogColl.Insert(op2), ShouldBeNil)

			// this one should be filtered out, since it occurred before the
			// threshold
			op3 := &Oplog{
				Timestamp: bson.MongoTimestamp(1<<62 - 1), // more than 1 day in the past
				HistoryID: 300,
				Version:   2,
				Operation: "i",
				Namespace: "mongooplog_test.data",
				Object: bson.M{
					"_id": 3,
				},
			}
			So(oplogColl.Insert(op3), ShouldBeNil)

			// initialize the mongooplog
			oplog := MongoOplog{
				ToolOptions:         opts,
				SourceOptions:       sourceOpts,
				SessionProviderFrom: sourceSP,
				CmdRunnerTo:         destSP,
			}

			// run it
			So(oplog.Run(), ShouldBeNil)

			// all the operations after the threshold should have been applied
			var inserted []bson.M
			So(testColl.Find(bson.M{}).Sort("_id").All(&inserted),
				ShouldBeNil)
			So(len(inserted), ShouldEqual, 2)
			So(inserted[0]["_id"], ShouldEqual, 1)
			So(inserted[1]["_id"], ShouldEqual, 2)

		})

		Convey("when using a non-default oplog", func() {

			Convey("all operations should be applied correctly, without"+
				" error", func() {

				// change the oplog to a different collection
				sourceOpts.OplogNS = "mongooplog_test.oplog"

				// initialize a session provider for the source
				sourceSP, err := db.InitSessionProvider(*opts)
				So(err, ShouldBeNil)

				// initialize a session provider for the destination
				destSP, err := db.InitSessionProvider(*opts)
				So(err, ShouldBeNil)

				// clear out the oplog
				sess, err := sourceSP.GetSession()
				So(err, ShouldBeNil)
				defer sess.Close()
				oplogColl := sess.DB("mongooplog_test").C("oplog")
				oplogColl.DropCollection()

				// create the oplog as a capped collection, so it can be tailed
				So(sess.DB("mongooplog_test").Run(
					bson.M{"create": "oplog", "capped": true, "size": 10000},
					bson.M{}),
					ShouldBeNil)

				// clear out the collection we'll use for testing
				testColl := sess.DB("mongooplog_test").C("data")
				_, err = testColl.RemoveAll(bson.M{})
				So(err, ShouldBeNil)

				// insert some "ops" into the oplog to be found and applied
				op1 := &Oplog{
					Timestamp: bson.MongoTimestamp(1<<63 - 1), // years in the future
					HistoryID: 100,
					Version:   2,
					Operation: "i",
					Namespace: "mongooplog_test.data",
					Object: bson.M{
						"_id": 3,
					},
				}
				So(oplogColl.Insert(op1), ShouldBeNil)
				op2 := &Oplog{
					Timestamp: bson.MongoTimestamp(1<<63 - 1), // years in the future
					HistoryID: 200,
					Version:   2,
					Operation: "i",
					Namespace: "mongooplog_test.data",
					Object: bson.M{
						"_id": 4,
					},
				}
				So(oplogColl.Insert(op2), ShouldBeNil)

				// initialize the mongooplog
				oplog := MongoOplog{
					ToolOptions:         opts,
					SourceOptions:       sourceOpts,
					SessionProviderFrom: sourceSP,
					CmdRunnerTo:         destSP,
				}

				// run it
				So(oplog.Run(), ShouldBeNil)

				// the operations should have been applied
				var inserted []bson.M
				So(testColl.Find(bson.M{}).Sort("_id").All(&inserted),
					ShouldBeNil)
				So(len(inserted), ShouldEqual, 2)
				So(inserted[0]["_id"], ShouldEqual, 3)
				So(inserted[1]["_id"], ShouldEqual, 4)

			})

		})

	})

}
