package json

import (
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestBooleanValue(t *testing.T) {

	Convey("When unmarshalling JSON with Boolean values", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean(123)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)
			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)
		})

		Convey("works for no args", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean()"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)
			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)
		})

		Convey("works for a struct of a specific type", func() {
			type TestStruct struct {
				A bool
				b int
			}
			var jsonStruct TestStruct

			key := "A"
			value := "Boolean(123)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonStruct)
			So(err, ShouldBeNil)
			So(jsonStruct.A, ShouldEqual, true)

			key = "A"
			value = "Boolean(0)"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonStruct)
			So(err, ShouldBeNil)
			So(jsonStruct.A, ShouldEqual, false)
		})

		Convey("works for bool", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean(true)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)

			value = "Boolean(false)"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)
		})

		Convey("works for numbers", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean(1)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)

			value = "Boolean(0)"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)

			value = "Boolean(0.0)"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)

			value = "Boolean(2.0)"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)

			value = "Boolean(-15.4)"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)
		})

		Convey("works for strings", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean('hello')"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)

			value = "Boolean('')"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)
		})

		Convey("works for undefined", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean(undefined)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)
		})

		Convey("works for null", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean(null)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)
		})

		Convey("works when given too many args", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean(true, false)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)

			key = "key"
			value = "Boolean(false, true)"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "Boolean(123)", "Boolean(0)", "Boolean(true)"
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldEqual, true)

			jsonValue2, ok := jsonMap[key2].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldEqual, false)

			jsonValue3, ok := jsonMap[key3].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldEqual, true)
		})

		Convey("works for other types", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean(new Date (0))"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)

			value = "Boolean(ObjectId('56609335028bd7dc5c36cb9f'))"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)

			value = "Boolean([])"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)
		})

		Convey("works for nested booleans", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean(Boolean(5))"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)

			value = "Boolean(Boolean(Boolean(0)))"
			data = fmt.Sprintf(`{"%v":%v}`, key, value)

			err = Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok = jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value1 := "Boolean(42)"
			value2 := "Boolean(0)"
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value1, value2, value1)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			jsonValue, ok := jsonArray[0].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)

			jsonValue, ok = jsonArray[1].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)

			jsonValue, ok = jsonArray[2].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)
		})

		Convey("can specify argument in hexadecimal (true)", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean(0x5f)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, true)
		})

		Convey("can specify argument in hexadecimal (false)", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Boolean(0x0)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(bool)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, false)
		})
	})
}
