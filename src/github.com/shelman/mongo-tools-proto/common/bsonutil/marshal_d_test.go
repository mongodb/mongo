package bsonutil

import (
	"encoding/json"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"strings"
	"testing"
)

func TestMarshalDMarshalJSON(t *testing.T) {

	Convey("With a valid bson.D", t, func() {
		testD := bson.D{
			{"cool", "rad"},
			{"aaa", 543.2},
			{"I", 0},
			{"E", 0},
			{"map", bson.M{"1": 1, "2": "two"}},
		}

		Convey("wrapping with MarshalD should allow json.Marshal to work", func() {
			asJSON, err := json.Marshal(MarshalD(testD))
			So(err, ShouldBeNil)
			strJSON := string(asJSON)

			Convey("with order preserved", func() {
				So(strings.Index(strJSON, "cool"), ShouldBeLessThan, strings.Index(strJSON, "aaa"))
				So(strings.Index(strJSON, "aaa"), ShouldBeLessThan, strings.Index(strJSON, "I"))
				So(strings.Index(strJSON, "I"), ShouldBeLessThan, strings.Index(strJSON, "E"))
				So(strings.Index(strJSON, "E"), ShouldBeLessThan, strings.Index(strJSON, "map"))
				So(strings.Count(strJSON, ","), ShouldEqual, 5) // 4 + 1 from internal map
			})

			Convey("but still usable by the json parser", func() {
				var asMap bson.M
				err := json.Unmarshal(asJSON, &asMap)
				So(err, ShouldBeNil)

				Convey("with types & values preserved", func() {
					So(asMap["cool"], ShouldEqual, "rad")
					So(asMap["aaa"], ShouldEqual, 543.2)
					So(asMap["I"], ShouldEqual, 0)
					So(asMap["E"], ShouldEqual, 0)
					So(asMap["map"].(map[string]interface{})["1"], ShouldEqual, 1)
					So(asMap["map"].(map[string]interface{})["2"], ShouldEqual, "two")
				})
			})

			Convey("putting it inside another map should still be usable by json.Marshal", func() {
				_, err := json.Marshal(bson.M{"x": 0, "y": MarshalD(testD)})
				So(err, ShouldBeNil)
			})
		})
	})

	Convey("With en empty bson.D", t, func() {
		testD := bson.D{}

		Convey("wrapping with MarshalD should allow json.Marshal to work", func() {
			asJSON, err := json.Marshal(MarshalD(testD))
			So(err, ShouldBeNil)
			strJSON := string(asJSON)
			So(strJSON, ShouldEqual, "{}")

			Convey("but still usable by the json parser", func() {
				var asInterface interface{}
				err := json.Unmarshal(asJSON, &asInterface)
				So(err, ShouldBeNil)
				asMap, ok := asInterface.(map[string]interface{})
				So(ok, ShouldBeTrue)
				So(len(asMap), ShouldEqual, 0)
			})
		})
	})
}
