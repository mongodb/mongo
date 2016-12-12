package bsonutil

import (
	"github.com/mongodb/mongo-tools/common/json"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestNumberLongValue(t *testing.T) {

	Convey("When converting JSON with NumberLong values", t, func() {

		Convey("works for NumberLong constructor", func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: json.NumberLong(42),
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldBeNil)
			So(jsonMap[key], ShouldEqual, int64(42))
		})

		Convey(`works for NumberLong document ('{ "$numberLong": "42" }')`, func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: map[string]interface{}{
					"$numberLong": "42",
				},
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldBeNil)
			So(jsonMap[key], ShouldEqual, int64(42))
		})
	})
}
