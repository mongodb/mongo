package json

import (
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestHexadecimalNumber(t *testing.T) {
	value := "0x123"
	intValue := 0x123

	Convey("When unmarshalling JSON with hexadecimal numeric values", t, func() {
		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}
			key := "key"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)
			jsonValue, ok := jsonMap[key].(int32)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, intValue)
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "0x100", "0x101", "0x102"
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(int32)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldEqual, 0x100)

			jsonValue2, ok := jsonMap[key2].(int32)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldEqual, 0x101)

			jsonValue3, ok := jsonMap[key3].(int32)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldEqual, 0x102)
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(int32)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldEqual, intValue)
			}
		})

		Convey("can have a sign ('+' or '-')", func() {
			var jsonMap map[string]interface{}

			key := "key"
			data := fmt.Sprintf(`{"%v":+%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(int32)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, intValue)

			data = fmt.Sprintf(`{"%v":-%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(int32)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, -intValue)
		})

		Convey("can use '0x' or '0X' prefix", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "123"
			data := fmt.Sprintf(`{"%v":0x%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(int32)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, intValue)

			data = fmt.Sprintf(`{"%v":0X%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(int32)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, intValue)
		})
	})
}
