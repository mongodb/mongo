package mongorestore

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

const ExistsDB = "restore_collection_exists"

func TestCollectionExists(t *testing.T) {

	testutil.VerifyTestType(t, testutil.INTEGRATION_TEST_TYPE)

	Convey("With a test mongorestore", t, func() {
		ssl := testutil.GetSSLOptions()
		auth := testutil.GetAuthOptions()
		sessionProvider, err := db.NewSessionProvider(commonOpts.ToolOptions{
			Connection: &commonOpts.Connection{
				Host: "localhost",
				Port: "27017",
			},
			Auth: &auth,
			SSL:  &ssl,
		})
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
