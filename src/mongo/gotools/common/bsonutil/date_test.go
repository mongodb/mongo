package bsonutil

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/json"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
	"time"
)

func TestDateValue(t *testing.T) {

	Convey("When converting JSON with Date values", t, func() {

		Convey("works for Date object", func() {
			key := "key"
			jsonMap := map[string]interface{}{
				key: json.Date(100),
			}

			err := ConvertJSONDocumentToBSON(jsonMap)
			So(err, ShouldBeNil)

			jsonValue, ok := jsonMap[key].(time.Time)
			So(ok, ShouldBeTrue)
			So(jsonValue.Equal(time.Unix(0, int64(100*time.Millisecond))), ShouldBeTrue)
		})

		Convey("works for Date document", func() {

			dates := []string{
				"2006-01-02T15:04:05.000Z",
				"2006-01-02T15:04:05.000-0700",
				"2006-01-02T15:04:05Z",
				"2006-01-02T15:04:05-0700",
				"2006-01-02T15:04Z",
				"2006-01-02T15:04-0700",
			}

			for _, dateString := range dates {
				example := fmt.Sprintf(`{ "$date": "%v" }`, dateString)
				Convey(fmt.Sprintf("of string ('%v')", example), func() {
					key := "key"
					jsonMap := map[string]interface{}{
						key: map[string]interface{}{
							"$date": dateString,
						},
					}

					err := ConvertJSONDocumentToBSON(jsonMap)
					So(err, ShouldBeNil)

					// dateString is a valid time format string
					date, err := time.Parse(dateString, dateString)
					So(err, ShouldBeNil)

					jsonValue, ok := jsonMap[key].(time.Time)
					So(ok, ShouldBeTrue)
					So(jsonValue.Equal(date), ShouldBeTrue)
				})
			}

			date := time.Unix(0, int64(time.Duration(1136214245000)*time.Millisecond))

			Convey(`of $numberLong ('{ "$date": { "$numberLong": "1136214245000" } }')`, func() {
				key := "key"
				jsonMap := map[string]interface{}{
					key: map[string]interface{}{
						"$date": map[string]interface{}{
							"$numberLong": "1136214245000",
						},
					},
				}

				err := ConvertJSONDocumentToBSON(jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(time.Time)
				So(ok, ShouldBeTrue)
				So(jsonValue.Equal(date), ShouldBeTrue)
			})

			Convey(`of json.Number ('{ "$date": 1136214245000 }')`, func() {
				key := "key"
				jsonMap := map[string]interface{}{
					key: map[string]interface{}{
						"$date": json.Number("1136214245000"),
					},
				}

				err := ConvertJSONDocumentToBSON(jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(time.Time)
				So(ok, ShouldBeTrue)
				So(jsonValue.Equal(date), ShouldBeTrue)
			})

			Convey(`of numeric int64 ('{ "$date": 1136214245000 }')`, func() {
				key := "key"
				jsonMap := map[string]interface{}{
					key: map[string]interface{}{
						"$date": int64(1136214245000),
					},
				}

				err := ConvertJSONDocumentToBSON(jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(time.Time)
				So(ok, ShouldBeTrue)
				So(jsonValue.Equal(date), ShouldBeTrue)
			})

			Convey(`of numeric float64 ('{ "$date": 1136214245000 }')`, func() {
				key := "key"
				jsonMap := map[string]interface{}{
					key: map[string]interface{}{
						"$date": float64(1136214245000),
					},
				}

				err := ConvertJSONDocumentToBSON(jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(time.Time)
				So(ok, ShouldBeTrue)
				So(jsonValue.Equal(date), ShouldBeTrue)
			})
			Convey(`of numeric int32 ('{ "$date": 2136800000 }')`, func() {
				key := "key"

				date = time.Unix(0, int64(time.Duration(2136800000)*time.Millisecond))

				jsonMap := map[string]interface{}{
					key: map[string]interface{}{
						"$date": int32(2136800000),
					},
				}

				err := ConvertJSONDocumentToBSON(jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(time.Time)
				So(ok, ShouldBeTrue)
				So(jsonValue.Equal(date), ShouldBeTrue)
			})

			Convey(`of negative numeric int32 ('{ "$date": -2136800000 }')`, func() {
				key := "key"

				date = time.Unix(0, int64(time.Duration(-2136800000)*time.Millisecond))

				jsonMap := map[string]interface{}{
					key: map[string]interface{}{
						"$date": int32(-2136800000),
					},
				}

				err := ConvertJSONDocumentToBSON(jsonMap)
				So(err, ShouldBeNil)

				jsonValue, ok := jsonMap[key].(time.Time)
				So(ok, ShouldBeTrue)
				So(jsonValue.Equal(date), ShouldBeTrue)
			})
		})
	})
}
