package json

import (
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestDateValue(t *testing.T) {

	Convey("When unmarshalling JSON with Date values", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Date(123)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(Date)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, Date(123))
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "Date(123)", "Date(456)", "Date(789)"
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue1, ok := jsonMap[key1].(Date)
			So(ok, ShouldBeTrue)
			So(jsonValue1, ShouldEqual, Date(123))

			jsonValue2, ok := jsonMap[key2].(Date)
			So(ok, ShouldBeTrue)
			So(jsonValue2, ShouldEqual, Date(456))

			jsonValue3, ok := jsonMap[key3].(Date)
			So(ok, ShouldBeTrue)
			So(jsonValue3, ShouldEqual, Date(789))
		})

		Convey("works in an array", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Date(42)"
			data := fmt.Sprintf(`{"%v":[%v,%v,%v]}`,
				key, value, value, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonArray, ok := jsonMap[key].([]interface{})
			So(ok, ShouldBeTrue)

			for _, _jsonValue := range jsonArray {
				jsonValue, ok := _jsonValue.(Date)
				So(ok, ShouldBeTrue)
				So(jsonValue, ShouldEqual, Date(42))
			}
		})

		Convey("cannot use string as argument", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := `Date("123")`
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldNotBeNil)
		})

		Convey("can specify argument in hexadecimal", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "Date(0x5f)"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(Date)
			So(ok, ShouldBeTrue)
			So(jsonValue, ShouldEqual, Date(0x5f))
		})
	})
}
