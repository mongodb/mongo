package mongooplog

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

func TestBasicOps(t *testing.T) {
	testutil.VerifyTestType(t, testutil.IntegrationTestType)

	var opts *options.ToolOptions
	var sourceOpts *SourceOptions

	Convey("When replicating operations", t, func() {
		ssl := testutil.GetSSLOptions()
		auth := testutil.GetAuthOptions()

		// specify localhost:33333 as the destination host
		opts = &options.ToolOptions{
			Namespace: &options.Namespace{},
			SSL:       &ssl,
			Auth:      &auth,
			Kerberos:  &options.Kerberos{},
			Connection: &options.Connection{
				Host: "localhost",
				Port: db.DefaultTestPort,
			},
		}

		// specify localhost:33333 as the source host
		sourceOpts = &SourceOptions{
			Seconds: 84600,            // the default
			OplogNS: "local.oplog.rs", // the default
		}

		Convey("all operations should be applied correctly, without"+
			" error", func() {

			// set the "oplog" we will use
			sourceOpts.OplogNS = "mongooplog_test.oplog"

			// initialize a session provider for the source
			sourceSP, err := db.NewSessionProvider(*opts)
			So(err, ShouldBeNil)

			// initialize a session provider for the destination
			destSP, err := db.NewSessionProvider(*opts)
			So(err, ShouldBeNil)

			// clear out the oplog
			sess, err := sourceSP.GetSession()
			So(err, ShouldBeNil)
			defer sess.Close()
			oplogColl := sess.DB("mongooplog_test").C("oplog")
			oplogColl.DropCollection()

			// create the oplog as a capped collection, so it can be tailed
			So(sess.DB("mongooplog_test").Run(
				bson.D{{"create", "oplog"}, {"capped", true},
					{"size", 10000}},
				bson.M{}),
				ShouldBeNil)

			// create the collection we are testing against (ignore errors)
			sess.DB("mongooplog_test").C("data").Create(&mgo.CollectionInfo{})

			// clear out the collection we'll use for testing
			testColl := sess.DB("mongooplog_test").C("data")
			_, err = testColl.RemoveAll(bson.M{})
			So(err, ShouldBeNil)

			// insert some "ops" into the oplog to be found and applied
			obj1 := bson.D{{"_id", 3}}
			op1 := &db.Oplog{
				Timestamp: bson.MongoTimestamp(1<<63 - 1), // years in the future
				HistoryID: 100,
				Version:   2,
				Operation: "i",
				Namespace: "mongooplog_test.data",
				Object:    obj1,
			}
			So(oplogColl.Insert(op1), ShouldBeNil)
			obj2 := bson.D{{"_id", 4}}
			op2 := &db.Oplog{
				Timestamp: bson.MongoTimestamp(1<<63 - 1), // years in the future
				HistoryID: 200,
				Version:   2,
				Operation: "i",
				Namespace: "mongooplog_test.data",
				Object:    obj2,
			}
			So(oplogColl.Insert(op2), ShouldBeNil)

			// this one should be filtered out, since it occurred before the
			// threshold
			obj3 := bson.D{{"_id", 3}}
			op3 := &db.Oplog{
				Timestamp: bson.MongoTimestamp(1<<62 - 1), // more than 1 day in the past
				HistoryID: 300,
				Version:   2,
				Operation: "i",
				Namespace: "mongooplog_test.data",
				Object:    obj3,
			}
			So(oplogColl.Insert(op3), ShouldBeNil)

			// initialize the mongooplog
			oplog := MongoOplog{
				ToolOptions:         opts,
				SourceOptions:       sourceOpts,
				SessionProviderFrom: sourceSP,
				SessionProviderTo:   destSP,
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

}
