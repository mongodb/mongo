package bsonutil

import (
	"github.com/mongodb/mongo-tools/common/json"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestNumberIntValue(t *testing.T) {

	Convey("When converting JSON with NumberInt values", t, func() {

		Convey("works for NumberInt constructor", func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: json.NumberInt(42),
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldBeNil)
			So(jsonMap[key], ShouldEqual, int32(42))
		})

		Convey(`works for NumberInt document ('{ "$numberInt": "42" }')`, func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: map[string]interface{}{
					"$numberInt": "42",
				},
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldBeNil)
			So(jsonMap[key], ShouldEqual, int32(42))
		})
	})
}
