package json

import (
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"math"
	"testing"
)

func TestDBRefValue(t *testing.T) {

	Convey("When unmarshalling JSON with DBRef values", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `DBRef("ref", "123")`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(DBRef)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, DBRef{"ref", "123", ""})
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := `DBRef("ref1", "123")`,
				`DBRef("ref2", "456")`, `DBRef("ref3", "789")`
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(DBRef)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldResemble, DBRef{"ref1", "123", ""})

			jsonValue2, ok := jsonMap[key2].(DBRef)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldResemble, DBRef{"ref2", "456", ""})

			jsonValue3, ok := jsonMap[key3].(DBRef)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldResemble, DBRef{"ref3", "789", ""})
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `DBRef("ref", "42")`
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", "42", ""})
			}
		})

		Convey("can use alternative capitalization ('Dbref')", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `Dbref("ref", "123")`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(DBRef)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldResemble, DBRef{"ref", "123", ""})
		})

		Convey("can have any extended JSON value for id parameter", func() {

			Convey("a null literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", null)`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", nil, ""})
			})

			Convey("a true literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", true)`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", true, ""})
			})

			Convey("a false literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", false)`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", false, ""})
			})

			Convey("an undefined literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", undefined)`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", Undefined{}, ""})
			})

			Convey("a NaN literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", NaN)`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue.Collection, ShouldEqual, "ref")

				id, ok := jsonValue.Id.(float64)
				So(ok, ShouldBeTrue)
				So(math.IsNaN(id), ShouldBeTrue)

			})

			Convey("an Infinity literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", Infinity)`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue.Collection, ShouldEqual, "ref")

				id, ok := jsonValue.Id.(float64)
				So(ok, ShouldBeTrue)
				So(math.IsInf(id, 1), ShouldBeTrue)

			})

			Convey("a MinKey literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", MinKey)`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", MinKey{}, ""})
			})

			Convey("a MaxKey literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", MaxKey)`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", MaxKey{}, ""})
			})

			Convey("an ObjectId object", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", ObjectId("123"))`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", ObjectId("123"), ""})
			})

			Convey("a NumberInt object", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", NumberInt(123))`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", NumberInt(123), ""})
			})

			Convey("a NumberLong object", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", NumberLong(123))`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", NumberLong(123), ""})
			})

			Convey("a RegExp object", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", RegExp("xyz", "i"))`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", RegExp{"xyz", "i"}, ""})
			})

			Convey("a regular expression literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", /xyz/i)`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", RegExp{"xyz", "i"}, ""})
			})

			Convey("a Timestamp object", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", Timestamp(123, 321))`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", Timestamp{123, 321}, ""})
			})

			Convey("a string literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", "xyz")`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldResemble, DBRef{"ref", "xyz", ""})
			})

			Convey("a numeric literal", func() {
				var jsonMap map[string]interface{}

				key := "key"
				value := `DBRef("ref", 123)`
				data := fmt.Sprintf(`{"%v":%v}`, key, value)

				err := Unmarshal([]byte(data), &jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(DBRef)
				So(ok, ShouldBeTrue)
				So(jsonValue.Collection, ShouldEqual, "ref")

				id, ok := jsonValue.Id.(int32)
				So(ok, ShouldBeTrue)
				So(id, ShouldAlmostEqual, 123)
			})
		})
	})
}
