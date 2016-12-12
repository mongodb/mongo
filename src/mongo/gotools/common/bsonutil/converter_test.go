package bsonutil

import (
	"encoding/base64"
	"fmt"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"testing"
	"time"
)

func TestObjectIdBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting a BSON ObjectId", t, func() {
		Convey("that is valid to JSON should produce a json.ObjectId", func() {
			bsonObjId := bson.NewObjectId()
			jsonObjId := json.ObjectId(bsonObjId.Hex())

			_jObjId, err := ConvertBSONValueToJSON(bsonObjId)
			So(err, ShouldBeNil)
			jObjId, ok := _jObjId.(json.ObjectId)
			So(ok, ShouldBeTrue)

			So(jObjId, ShouldNotEqual, bsonObjId)
			So(jObjId, ShouldEqual, jsonObjId)
		})
	})
}

func TestArraysBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting BSON arrays to JSON arrays", t, func() {
		Convey("should work for empty arrays", func() {
			jArr, err := ConvertBSONValueToJSON([]interface{}{})
			So(err, ShouldBeNil)

			So(jArr, ShouldResemble, []interface{}{})
		})

		Convey("should work for one-level deep arrays", func() {
			objId := bson.NewObjectId()
			bsonArr := []interface{}{objId, 28, 0.999, "plain"}
			_jArr, err := ConvertBSONValueToJSON(bsonArr)
			So(err, ShouldBeNil)
			jArr, ok := _jArr.([]interface{})
			So(ok, ShouldBeTrue)

			So(len(jArr), ShouldEqual, 4)
			So(jArr[0], ShouldEqual, json.ObjectId(objId.Hex()))
			So(jArr[1], ShouldEqual, 28)
			So(jArr[2], ShouldEqual, 0.999)
			So(jArr[3], ShouldEqual, "plain")
		})

		Convey("should work for arrays with embedded objects", func() {
			bsonObj := []interface{}{
				80,
				bson.M{
					"a": int64(20),
					"b": bson.M{
						"c": bson.RegEx{Pattern: "hi", Options: "i"},
					},
				},
			}

			__jObj, err := ConvertBSONValueToJSON(bsonObj)
			So(err, ShouldBeNil)
			_jObj, ok := __jObj.([]interface{})
			So(ok, ShouldBeTrue)
			jObj, ok := _jObj[1].(bson.M)
			So(ok, ShouldBeTrue)
			So(len(jObj), ShouldEqual, 2)
			So(jObj["a"], ShouldEqual, json.NumberLong(20))
			jjObj, ok := jObj["b"].(bson.M)
			So(ok, ShouldBeTrue)

			So(jjObj["c"], ShouldResemble, json.RegExp{"hi", "i"})
			So(jjObj["c"], ShouldNotResemble, json.RegExp{"i", "hi"})
		})

	})
}

func TestDateBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	timeNow := time.Now()
	secs := int64(timeNow.Unix())
	nanosecs := timeNow.Nanosecond()
	millis := int64(nanosecs / 1e6)

	timeNowSecs := time.Unix(secs, int64(0))
	timeNowMillis := time.Unix(secs, int64(millis*1e6))

	Convey("Converting BSON time.Time 's dates to JSON", t, func() {
		// json.Date is stored as an int64 representing the number of milliseconds since the epoch
		Convey(fmt.Sprintf("should work with second granularity: %v", timeNowSecs), func() {
			_jObj, err := ConvertBSONValueToJSON(timeNowSecs)
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.Date)
			So(ok, ShouldBeTrue)

			So(int64(jObj), ShouldEqual, secs*1e3)
		})

		Convey(fmt.Sprintf("should work with millisecond granularity: %v", timeNowMillis), func() {
			_jObj, err := ConvertBSONValueToJSON(timeNowMillis)
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.Date)
			So(ok, ShouldBeTrue)

			So(int64(jObj), ShouldEqual, secs*1e3+millis)
		})

		Convey(fmt.Sprintf("should work with nanosecond granularity: %v", timeNow), func() {
			_jObj, err := ConvertBSONValueToJSON(timeNow)
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.Date)
			So(ok, ShouldBeTrue)

			// we lose nanosecond precision
			So(int64(jObj), ShouldEqual, secs*1e3+millis)
		})

	})
}

func TestMaxKeyBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting a BSON Maxkey to JSON", t, func() {
		Convey("should produce a json.MaxKey", func() {
			_jObj, err := ConvertBSONValueToJSON(bson.MaxKey)
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.MaxKey)
			So(ok, ShouldBeTrue)

			So(jObj, ShouldResemble, json.MaxKey{})
		})
	})
}

func TestMinKeyBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting a BSON Maxkey to JSON", t, func() {
		Convey("should produce a json.MinKey", func() {
			_jObj, err := ConvertBSONValueToJSON(bson.MinKey)
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.MinKey)
			So(ok, ShouldBeTrue)

			So(jObj, ShouldResemble, json.MinKey{})
		})
	})
}

func Test64BitIntBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting a BSON int64 to JSON", t, func() {
		Convey("should produce a json.NumberLong", func() {
			_jObj, err := ConvertBSONValueToJSON(int32(243))
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.NumberInt)
			So(ok, ShouldBeTrue)

			So(jObj, ShouldEqual, json.NumberInt(243))
		})
	})

}

func Test32BitIntBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting a BSON int32 integer to JSON", t, func() {
		Convey("should produce a json.NumberInt", func() {
			_jObj, err := ConvertBSONValueToJSON(int64(888234334343))
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.NumberLong)
			So(ok, ShouldBeTrue)

			So(jObj, ShouldEqual, json.NumberLong(888234334343))
		})
	})

}

func TestRegExBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting a BSON Regular Expression (= /decision/gi) to JSON", t, func() {
		Convey("should produce a json.RegExp", func() {
			_jObj, err := ConvertBSONValueToJSON(bson.RegEx{"decision", "gi"})
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.RegExp)
			So(ok, ShouldBeTrue)

			So(jObj, ShouldResemble, json.RegExp{"decision", "gi"})
		})
	})

}

func TestUndefinedValueBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting a BSON Undefined type to JSON", t, func() {
		Convey("should produce a json.Undefined", func() {
			_jObj, err := ConvertBSONValueToJSON(bson.Undefined)
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.Undefined)
			So(ok, ShouldBeTrue)

			So(jObj, ShouldResemble, json.Undefined{})
		})
	})
}

func TestDBRefBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting BSON DBRef to JSON", t, func() {
		Convey("should produce a json.DBRef", func() {
			_jObj, err := ConvertBSONValueToJSON(mgo.DBRef{"coll1", "some_id", "test"})
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.DBRef)
			So(ok, ShouldBeTrue)

			So(jObj, ShouldResemble, json.DBRef{"coll1", "some_id", "test"})
			So(jObj, ShouldNotResemble, json.DBRef{"coll1", "test", "some_id"})
		})
	})
}

func TestTimestampBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting a BSON Timestamp to JSON", t, func() {
		Convey("should produce a json.Timestamp", func() {
			// {t:803434343, i:9} == bson.MongoTimestamp(803434343*2**32 + 9)
			_jObj, err := ConvertBSONValueToJSON(bson.MongoTimestamp(uint64(803434343<<32) | uint64(9)))
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.Timestamp)
			So(ok, ShouldBeTrue)

			So(jObj, ShouldResemble, json.Timestamp{Seconds: 803434343, Increment: 9})
			So(jObj, ShouldNotResemble, json.Timestamp{Seconds: 803434343, Increment: 8})
		})
	})
}

func TestBinaryBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting BSON Binary data to JSON", t, func() {
		Convey("should produce a json.BinData", func() {
			_jObj, err := ConvertBSONValueToJSON(bson.Binary{'\x01', []byte("\x05\x20\x02\xae\xf7")})
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.BinData)
			So(ok, ShouldBeTrue)

			base64data1 := base64.StdEncoding.EncodeToString([]byte("\x05\x20\x02\xae\xf7"))
			base64data2 := base64.StdEncoding.EncodeToString([]byte("\x05\x20\x02\xaf\xf7"))
			So(jObj, ShouldResemble, json.BinData{'\x01', base64data1})
			So(jObj, ShouldNotResemble, json.BinData{'\x01', base64data2})
		})
	})
}

func TestGenericBytesBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting Go bytes to JSON", t, func() {
		Convey("should produce a json.BinData with Type=0x00 (Generic)", func() {
			_jObj, err := ConvertBSONValueToJSON([]byte("this is something that's cool"))
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.BinData)
			So(ok, ShouldBeTrue)

			base64data := base64.StdEncoding.EncodeToString([]byte("this is something that's cool"))
			So(jObj, ShouldResemble, json.BinData{0x00, base64data})
			So(jObj, ShouldNotResemble, json.BinData{0x01, base64data})
		})
	})
}

func TestUnknownBSONTypeToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting an unknown BSON type to JSON", t, func() {
		Convey("should produce an error", func() {
			_, err := ConvertBSONValueToJSON(func() {})
			So(err, ShouldNotBeNil)
		})
	})
}

func TestDBPointerBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting BSON DBPointer to JSON", t, func() {
		Convey("should produce a json.DBPointer", func() {
			objId := bson.NewObjectId()
			_jObj, err := ConvertBSONValueToJSON(bson.DBPointer{"dbrefnamespace", objId})
			So(err, ShouldBeNil)
			jObj, ok := _jObj.(json.DBPointer)
			So(ok, ShouldBeTrue)

			So(jObj, ShouldResemble, json.DBPointer{"dbrefnamespace", objId})
		})
	})
}

func TestJSCodeBSONToJSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Converting BSON Javascript code to JSON", t, func() {
		Convey("should produce a json.Javascript", func() {
			Convey("without scope if the scope for the BSON Javascript code is nil", func() {
				_jObj, err := ConvertBSONValueToJSON(bson.JavaScript{"function() { return null; }", nil})
				So(err, ShouldBeNil)
				jObj, ok := _jObj.(json.JavaScript)
				So(ok, ShouldBeTrue)

				So(jObj, ShouldResemble, json.JavaScript{"function() { return null; }", nil})
			})

			Convey("with scope if the scope for the BSON Javascript code is non-nil", func() {
				_jObj, err := ConvertBSONValueToJSON(bson.JavaScript{"function() { return x; }", bson.M{"x": 2}})
				So(err, ShouldBeNil)
				jObj, ok := _jObj.(json.JavaScript)
				So(ok, ShouldBeTrue)
				So(jObj.Scope.(bson.M)["x"], ShouldEqual, 2)
				So(jObj.Code, ShouldEqual, "function() { return x; }")
			})
		})
	})
}
