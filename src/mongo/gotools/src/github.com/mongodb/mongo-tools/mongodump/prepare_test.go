package mongodump

import (
	"testing"

	"github.com/mongodb/mongo-tools/common/testtype"
	. "github.com/smartystreets/goconvey/convey"

	"gopkg.in/mgo.v2/bson"
)

func TestSkipCollection(t *testing.T) {

	testtype.VerifyTestType(t, testtype.UnitTestType)

	Convey("With a mongodump that excludes collections 'test' and 'fake'"+
		" and excludes prefixes 'pre-' and 'no'", t, func() {
		md := &MongoDump{
			OutputOptions: &OutputOptions{
				ExcludedCollections:        []string{"test", "fake"},
				ExcludedCollectionPrefixes: []string{"pre-", "no"},
			},
		}

		Convey("collection 'pre-test' should be skipped", func() {
			So(md.shouldSkipCollection("pre-test"), ShouldBeTrue)
		})

		Convey("collection 'notest' should be skipped", func() {
			So(md.shouldSkipCollection("notest"), ShouldBeTrue)
		})

		Convey("collection 'test' should be skipped", func() {
			So(md.shouldSkipCollection("test"), ShouldBeTrue)
		})

		Convey("collection 'fake' should be skipped", func() {
			So(md.shouldSkipCollection("fake"), ShouldBeTrue)
		})

		Convey("collection 'fake222' should not be skipped", func() {
			So(md.shouldSkipCollection("fake222"), ShouldBeFalse)
		})

		Convey("collection 'random' should not be skipped", func() {
			So(md.shouldSkipCollection("random"), ShouldBeFalse)
		})

		Convey("collection 'mytest' should not be skipped", func() {
			So(md.shouldSkipCollection("mytest"), ShouldBeFalse)
		})
	})

}

var (
	testSystemDB = "sysjstestdb"
)

func setUpTestSystemJS() error {
	session, err := getBareSession()
	if err != nil {
		return err
	}
	defer session.Close()

	collectionName := "system.js"

	coll := session.DB(testSystemDB).C(collectionName)

	err = coll.Insert(bson.M{"_id": "echoFunction", "value": "function(x) { return x; }"})
	if err != nil {
		return err
	}

	return nil
}

func TestCreateIntentsForDatabase(t *testing.T) {
	testtype.VerifyTestType(t, testtype.IntegrationTestType)

	Convey("With a MongoDump instance create an intent for system.js", t, func() {
		err := setUpTestSystemJS()
		So(err, ShouldBeNil)

		md := simpleMongoDumpInstance()
		md.InputOptions.Query = ""

		md.ToolOptions.Namespace.Collection = "system.js"
		err = md.Init()
		So(err, ShouldBeNil)

		err = md.CreateIntentsForDatabase(testSystemDB)
		So(err, ShouldBeNil)
		So(len(md.manager.Intents()), ShouldEqual, 1)

	})
}
