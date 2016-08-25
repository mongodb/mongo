package json

import (
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestUnquotedKeys(t *testing.T) {

	Convey("When unmarshalling JSON without quotes around its keys", t, func() {

		Convey("works for a single key", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "value"
			data := fmt.Sprintf(`{%v:"%v"}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			So(jsonMap[key], ShouldEqual, value)
		})

		Convey("works for multiple keys", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "value1", "value2", "value3"
			data := fmt.Sprintf(`{%v:"%v",%v:"%v",%v:"%v"}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			So(jsonMap[key1], ShouldEqual, value1)
			So(jsonMap[key2], ShouldEqual, value2)
			So(jsonMap[key3], ShouldEqual, value3)
		})

		Convey("can start with a dollar sign ('$')", func() {
			var jsonMap map[string]interface{}

			key := "$dollar"
			value := "money"
			data := fmt.Sprintf(`{%v:"%v"}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			So(jsonMap[key], ShouldEqual, value)
		})

		Convey("can start with an underscore ('_')", func() {
			var jsonMap map[string]interface{}

			key := "_id"
			value := "unique"
			data := fmt.Sprintf(`{%v:"%v"}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			So(jsonMap[key], ShouldEqual, value)
		})

		Convey("cannot start with a number ('[0-9]')", func() {
			var jsonMap map[string]interface{}

			key := "073"
			value := "octal"
			data := fmt.Sprintf(`{%v:"%v"}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldNotBeNil)
		})

		Convey("can contain numbers ('[0-9]')", func() {
			var jsonMap map[string]interface{}

			key := "b16"
			value := "little"
			data := fmt.Sprintf(`{%v:"%v"}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldBeNil)

			So(jsonMap[key], ShouldEqual, value)
		})

		Convey("cannot contain a period ('.')", func() {
			var jsonMap map[string]interface{}

			key := "horse.horse"
			value := "horse"
			data := fmt.Sprintf(`{%v:"%v"}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldNotBeNil)
		})
	})

	Convey("When unmarshalling JSON without quotes around its values", t, func() {

		Convey("fails for a single value", func() {
			var jsonMap map[string]interface{}

			key := "key"
			value := "value"
			data := fmt.Sprintf(`{"%v":%v}`, key, value)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldNotBeNil)
		})

		Convey("fails for multiple values", func() {
			var jsonMap map[string]interface{}

			key1, key2, key3 := "key1", "key2", "key3"
			value1, value2, value3 := "value1", "value2", "value3"
			data := fmt.Sprintf(`{"%v":%v,"%v":%v,"%v":%v}`,
				key1, value1, key2, value2, key3, value3)

			err := Unmarshal([]byte(data), &jsonMap)
			So(err, ShouldNotBeNil)
		})
	})
}
