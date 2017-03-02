package json

import (
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

func TestDecodeBsonD(t *testing.T) {
	Convey("When unmarshalling JSON into a bson.D", t, func() {
		Convey("a document should be stored with keys in the same order", func() {
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

		Convey("a nested bson.D should be parsed", func() {
			data := `{"a": 17, "b":{"foo":"bar", "baz":"boo"}, c:"wow" }`
			out := struct {
				A int    `json:"a"`
				B bson.D `json:"b"`
				C string `json:"c"`
			}{}
			err := Unmarshal([]byte(data), &out)
			So(err, ShouldBeNil)
			So(out.A, ShouldEqual, 17)
			So(out.C, ShouldEqual, "wow")
			So(len(out.B), ShouldEqual, 2)
			So(out.B[0].Name, ShouldEqual, "foo")
			So(out.B[0].Value, ShouldEqual, "bar")
			So(out.B[1].Name, ShouldEqual, "baz")
			So(out.B[1].Value, ShouldEqual, "boo")
		})

		Convey("objects nested within DocElems should still be parsed", func() {
			data := `{"a":["x", "y","z"], "b":{"foo":"bar", "baz":"boo"}}`
			out := bson.D{}
			err := Unmarshal([]byte(data), &out)
			So(err, ShouldBeNil)
			So(len(out), ShouldEqual, 2)
			So(out[0].Name, ShouldEqual, "a")
			So(out[1].Name, ShouldEqual, "b")
			So(out[0].Value, ShouldResemble, []interface{}{"x", "y", "z"})
			So(out[1].Value, ShouldResemble, bson.D{{"foo", "bar"}, {"baz", "boo"}})
		})

		Convey("only subdocuments inside a bson.D should be parsed into a bson.D", func() {
			data := `{subA: {a:{b:{c:9}}}, subB:{a:{b:{c:9}}}}`
			out := struct {
				A interface{} `json:"subA"`
				B bson.D      `json:"subB"`
			}{}
			err := Unmarshal([]byte(data), &out)
			So(err, ShouldBeNil)
			aMap := out.A.(map[string]interface{})
			So(len(aMap), ShouldEqual, 1)
			aMapSub := aMap["a"].(map[string]interface{})
			So(len(aMapSub), ShouldEqual, 1)
			aMapSubSub := aMapSub["b"].(map[string]interface{})
			So(aMapSubSub["c"], ShouldEqual, 9)
			So(len(out.B), ShouldEqual, 1)
			// using string comparison for simplicity
			c := bson.D{{Name: "c", Value: 9}}
			b := bson.D{{Name: "b", Value: c}}
			a := bson.D{{Name: "a", Value: b}}
			So(fmt.Sprintf("%v", out.B), ShouldEqual, fmt.Sprintf("%v", a))
		})

		Convey("subdocuments inside arrays inside bson.D should be parsed into a bson.D", func() {
			data := `{"a":[1,2,{b:"inner"}]}`
			out := bson.D{}
			err := Unmarshal([]byte(data), &out)
			So(err, ShouldBeNil)
			So(len(out), ShouldEqual, 1)
			So(out[0].Value, ShouldHaveSameTypeAs, []interface{}{})
			innerArray := out[0].Value.([]interface{})
			So(len(innerArray), ShouldEqual, 3)
			So(innerArray[0], ShouldEqual, 1)
			So(innerArray[1], ShouldEqual, 2)
			So(innerArray[2], ShouldHaveSameTypeAs, bson.D{})
			innerD := innerArray[2].(bson.D)
			So(len(innerD), ShouldEqual, 1)
			So(innerD[0].Name, ShouldEqual, "b")
			So(innerD[0].Value, ShouldEqual, "inner")
		})

		Convey("null should be a valid value", func() {
			data := `{"a":true, "b":null, "c": 5}`
			out := bson.D{}
			err := Unmarshal([]byte(data), &out)
			So(err, ShouldBeNil)
			So(len(out), ShouldEqual, 3)
			So(out[0].Name, ShouldEqual, "a")
			So(out[0].Value, ShouldEqual, true)
			So(out[1].Name, ShouldEqual, "b")
			So(out[1].Value, ShouldBeNil)
			So(out[2].Name, ShouldEqual, "c")
			So(out[2].Value, ShouldEqual, 5)
		})

	})
	Convey("Unmarshalling to a non-bson.D slice types should fail", t, func() {
		data := `{"a":["x", "y","z"], "b":{"foo":"bar", "baz":"boo"}}`
		out := []interface{}{}
		err := Unmarshal([]byte(data), &out)
		So(err, ShouldNotBeNil)
	})
}
