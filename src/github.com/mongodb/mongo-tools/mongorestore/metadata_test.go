package mongorestore

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

//TODO TODO TODO
func TestCollectionInfoFromOptions(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("With some test options bson examples", t, func() {
		//TODO stop using mgo :(

	})

}

func TestIndexDocumentConversion(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("With some test index doc bson examples", t, func() {

		Convey("simple index {a:1, b:-1}", func() {
			idx := IndexDocument{
				Name: "test1",
				Key:  bson.D{{"a", 1.0}, {"b", -1.0}},
			}

			Convey("converts properly", func() {
				mgoIdx, err := idx.ToMgoFormat()
				So(err, ShouldBeNil)
				So(mgoIdx.Name, ShouldEqual, "test1")
				So(len(mgoIdx.Key), ShouldEqual, 2)
				So(mgoIdx.Key[0], ShouldEqual, "+a")
				So(mgoIdx.Key[1], ShouldEqual, "-b")
			})
		})

		//TODO more of these after change to not use mgo
	})
}
