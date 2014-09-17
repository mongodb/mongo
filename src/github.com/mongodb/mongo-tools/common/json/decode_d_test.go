package json

import (
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

func TestDecodeBsonD(t *testing.T) {
	Convey("When unmarshalling JSON into a bson.D", t, func() {
		Convey("Should be stored as DocElem's with keys in the same order", func() {
			data := `{"a":1, "b":2, "c":3, "d":4, "e":5, "f":6}`
			out := bson.D{}
			err := Unmarshal([]byte(data), &out)
			So(err, ShouldBeNil)
			So(len(out), ShouldEqual, 6)
			So(out[0].Name, ShouldEqual, "a")
			So(out[1].Name, ShouldEqual, "b")
			So(out[2].Name, ShouldEqual, "c")
			So(out[3].Name, ShouldEqual, "d")
			So(out[4].Name, ShouldEqual, "e")
			So(out[5].Name, ShouldEqual, "f")

		})

		Convey("Objects nested within DocElems should still be parsed", func() {
			data := `{"a":["x", "y","z"], "b":{"foo":"bar", "baz":"boo"}}`
			out := bson.D{}
			err := Unmarshal([]byte(data), &out)
			So(err, ShouldBeNil)
			So(len(out), ShouldEqual, 2)
			So(out[0].Name, ShouldEqual, "a")
			So(out[1].Name, ShouldEqual, "b")
			So(out[0].Value, ShouldResemble, []interface{}{"x", "y", "z"})
			So(out[1].Value, ShouldResemble, map[string]interface{}{"foo": "bar", "baz": "boo"})
		})
	})
	Convey("Unmarshalling to a non-bson.D slice types should fail", t, func() {
		data := `{"a":["x", "y","z"], "b":{"foo":"bar", "baz":"boo"}}`
		out := []interface{}{}
		err := Unmarshal([]byte(data), &out)
		So(err, ShouldNotBeNil)
	})
}
