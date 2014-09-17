package json

import (
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestHexadecimalNumber(t *testing.T) {

	Convey("When unmarshalling JSON with hexadecimal numeric values", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "0x123"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(int64)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, 0x123)
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "0x123", "0x456", "0x789"
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(int64)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldEqual, 0x123)

			jsonValue2, ok := jsonMap[key2].(int64)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldEqual, 0x456)

			jsonValue3, ok := jsonMap[key3].(int64)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldEqual, 0x789)
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "0x42"
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(int64)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldEqual, 0x42)
			}
		})

		Convey("can have a sign ('+' or '-')", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "0x106"
			data := fmt.Sprintf(`{"%v":+%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(int64)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, 0x106)

			data = fmt.Sprintf(`{"%v":-%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(int64)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, -0x106)
		})

		Convey("can use '0x' or '0X' prefix", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "106"
			data := fmt.Sprintf(`{"%v":0x%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(int64)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, 0x106)

			data = fmt.Sprintf(`{"%v":0X%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(int64)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, 0x106)
		})
	})
}
