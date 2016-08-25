package mongoimport

import (
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
	"time"
)

func init() {
	log.SetVerbosity(&options.Verbosity{
		VLevel: 4,
	})
}

func TestTypedHeaderParser(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Using 'zip.string(),number.double(),foo.auto()'", t, func() {
		var headers = []string{"zip.string()", "number.double()", "foo.auto()", `bar.date(January 2\, \(2006\))`}
		var colSpecs []ColumnSpec
		var err error

		Convey("with parse grace: auto", func() {
			colSpecs, err = ParseTypedHeaders(headers, pgAutoCast)
			So(colSpecs, ShouldResemble, []ColumnSpec{
				{"zip", new(FieldStringParser), pgAutoCast, "string"},
				{"number", new(FieldDoubleParser), pgAutoCast, "double"},
				{"foo", new(FieldAutoParser), pgAutoCast, "auto"},
				{"bar", &FieldDateParser{"January 2, (2006)"}, pgAutoCast, "date"},
			})
			So(err, ShouldBeNil)
		})
		Convey("with parse grace: skipRow", func() {
			colSpecs, err = ParseTypedHeaders(headers, pgSkipRow)
			So(colSpecs, ShouldResemble, []ColumnSpec{
				{"zip", new(FieldStringParser), pgSkipRow, "string"},
				{"number", new(FieldDoubleParser), pgSkipRow, "double"},
				{"foo", new(FieldAutoParser), pgSkipRow, "auto"},
				{"bar", &FieldDateParser{"January 2, (2006)"}, pgSkipRow, "date"},
			})
			So(err, ShouldBeNil)
		})
	})

	Convey("Using various bad headers", t, func() {
		var err error

		Convey("with non-empty arguments for types that don't want them", func() {
			_, err = ParseTypedHeader("zip.string(blah)", pgAutoCast)
			So(err, ShouldNotBeNil)
			_, err = ParseTypedHeader("zip.string(0)", pgAutoCast)
			So(err, ShouldNotBeNil)
			_, err = ParseTypedHeader("zip.int32(0)", pgAutoCast)
			So(err, ShouldNotBeNil)
			_, err = ParseTypedHeader("zip.int64(0)", pgAutoCast)
			So(err, ShouldNotBeNil)
			_, err = ParseTypedHeader("zip.double(0)", pgAutoCast)
			So(err, ShouldNotBeNil)
			_, err = ParseTypedHeader("zip.auto(0)", pgAutoCast)
			So(err, ShouldNotBeNil)
		})
		Convey("with bad arguments for the binary type", func() {
			_, err = ParseTypedHeader("zip.binary(blah)", pgAutoCast)
			So(err, ShouldNotBeNil)
			_, err = ParseTypedHeader("zip.binary(binary)", pgAutoCast)
			So(err, ShouldNotBeNil)
			_, err = ParseTypedHeader("zip.binary(decimal)", pgAutoCast)
			So(err, ShouldNotBeNil)
		})
	})
}

func TestAutoHeaderParser(t *testing.T) {
	Convey("Using 'zip,number'", t, func() {
		var headers = []string{"zip", "number", "foo"}
		var colSpecs = ParseAutoHeaders(headers)
		So(colSpecs, ShouldResemble, []ColumnSpec{
			{"zip", new(FieldAutoParser), pgAutoCast, "auto"},
			{"number", new(FieldAutoParser), pgAutoCast, "auto"},
			{"foo", new(FieldAutoParser), pgAutoCast, "auto"},
		})
	})
}

func TestFieldParsers(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Using FieldAutoParser", t, func() {
		var p, _ = NewFieldParser(ctAuto, "")
		var value interface{}
		var err error

		Convey("parses integers when it can", func() {
			value, err = p.Parse("2147483648")
			So(value.(int64), ShouldEqual, int64(2147483648))
			So(err, ShouldBeNil)
			value, err = p.Parse("42")
			So(value.(int32), ShouldEqual, 42)
			So(err, ShouldBeNil)
			value, err = p.Parse("-2147483649")
			So(value.(int64), ShouldEqual, int64(-2147483649))
		})
		Convey("parses decimals when it can", func() {
			value, err = p.Parse("3.14159265")
			So(value.(float64), ShouldEqual, 3.14159265)
			So(err, ShouldBeNil)
			value, err = p.Parse("0.123123")
			So(value.(float64), ShouldEqual, 0.123123)
			So(err, ShouldBeNil)
			value, err = p.Parse("-123456.789")
			So(value.(float64), ShouldEqual, -123456.789)
			So(err, ShouldBeNil)
			value, err = p.Parse("-1.")
			So(value.(float64), ShouldEqual, -1.0)
			So(err, ShouldBeNil)
		})
		Convey("leaves everything else as a string", func() {
			value, err = p.Parse("12345-6789")
			So(value.(string), ShouldEqual, "12345-6789")
			So(err, ShouldBeNil)
			value, err = p.Parse("06/02/1997")
			So(value.(string), ShouldEqual, "06/02/1997")
			So(err, ShouldBeNil)
			value, err = p.Parse("")
			So(value.(string), ShouldEqual, "")
			So(err, ShouldBeNil)
		})
	})

	Convey("Using FieldBooleanParser", t, func() {
		var p, _ = NewFieldParser(ctBoolean, "")
		var value interface{}
		var err error

		Convey("parses representations of true correctly", func() {
			value, err = p.Parse("true")
			So(value.(bool), ShouldBeTrue)
			So(err, ShouldBeNil)
			value, err = p.Parse("TrUe")
			So(value.(bool), ShouldBeTrue)
			So(err, ShouldBeNil)
			value, err = p.Parse("1")
			So(value.(bool), ShouldBeTrue)
			So(err, ShouldBeNil)
		})
		Convey("parses representations of false correctly", func() {
			value, err = p.Parse("false")
			So(value.(bool), ShouldBeFalse)
			So(err, ShouldBeNil)
			value, err = p.Parse("FaLsE")
			So(value.(bool), ShouldBeFalse)
			So(err, ShouldBeNil)
			value, err = p.Parse("0")
			So(value.(bool), ShouldBeFalse)
			So(err, ShouldBeNil)
		})
		Convey("does not parse other boolean representations", func() {
			_, err = p.Parse("")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("t")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("f")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("yes")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("no")
			So(err, ShouldNotBeNil)
		})
	})

	Convey("Using FieldBinaryParser", t, func() {
		var value interface{}
		var err error

		Convey("using hex encoding", func() {
			var p, _ = NewFieldParser(ctBinary, "hex")
			Convey("parses valid hex values correctly", func() {
				value, err = p.Parse("400a11")
				So(value.([]byte), ShouldResemble, []byte{64, 10, 17})
				So(err, ShouldBeNil)
				value, err = p.Parse("400A11")
				So(value.([]byte), ShouldResemble, []byte{64, 10, 17})
				So(err, ShouldBeNil)
				value, err = p.Parse("0b400A11")
				So(value.([]byte), ShouldResemble, []byte{11, 64, 10, 17})
				So(err, ShouldBeNil)
				value, err = p.Parse("")
				So(value.([]byte), ShouldResemble, []byte{})
				So(err, ShouldBeNil)
			})
		})
		Convey("using base32 encoding", func() {
			var p, _ = NewFieldParser(ctBinary, "base32")
			Convey("parses valid base32 values correctly", func() {
				value, err = p.Parse("")
				So(value.([]uint8), ShouldResemble, []uint8{})
				So(err, ShouldBeNil)
				value, err = p.Parse("MZXW6YTBOI======")
				So(value.([]uint8), ShouldResemble, []uint8{102, 111, 111, 98, 97, 114})
				So(err, ShouldBeNil)
			})
		})
		Convey("using base64 encoding", func() {
			var p, _ = NewFieldParser(ctBinary, "base64")
			Convey("parses valid base64 values correctly", func() {
				value, err = p.Parse("")
				So(value.([]uint8), ShouldResemble, []uint8{})
				So(err, ShouldBeNil)
				value, err = p.Parse("Zm9vYmFy")
				So(value.([]uint8), ShouldResemble, []uint8{102, 111, 111, 98, 97, 114})
				So(err, ShouldBeNil)
			})
		})
	})

	Convey("Using FieldDateParser", t, func() {
		var value interface{}
		var err error

		Convey("with Go's format", func() {
			var p, _ = NewFieldParser(ctDateGo, "01/02/2006 3:04:05pm MST")
			Convey("parses valid timestamps correctly", func() {
				value, err = p.Parse("01/04/2000 5:38:10pm UTC")
				So(value.(time.Time), ShouldResemble, time.Date(2000, 1, 4, 17, 38, 10, 0, time.UTC))
				So(err, ShouldBeNil)
			})
			Convey("does not parse invalid dates", func() {
				_, err = p.Parse("01/04/2000 5:38:10pm")
				So(err, ShouldNotBeNil)
				_, err = p.Parse("01/04/2000 5:38:10 pm UTC")
				So(err, ShouldNotBeNil)
				_, err = p.Parse("01/04/2000")
				So(err, ShouldNotBeNil)
			})
		})
		Convey("with MS's format", func() {
			var p, _ = NewFieldParser(ctDateMS, "MM/dd/yyyy h:mm:sstt")
			Convey("parses valid timestamps correctly", func() {
				value, err = p.Parse("01/04/2000 5:38:10PM")
				So(value.(time.Time), ShouldResemble, time.Date(2000, 1, 4, 17, 38, 10, 0, time.UTC))
				So(err, ShouldBeNil)
			})
			Convey("does not parse invalid dates", func() {
				_, err = p.Parse("01/04/2000 :) 05:38:10PM")
				So(err, ShouldNotBeNil)
				_, err = p.Parse("01/04/2000 005:38:10PM")
				So(err, ShouldNotBeNil)
				_, err = p.Parse("01/04/2000 5:38:10 PM")
				So(err, ShouldNotBeNil)
				_, err = p.Parse("01/04/2000")
				So(err, ShouldNotBeNil)
			})
		})
		Convey("with Oracle's format", func() {
			var p, _ = NewFieldParser(ctDateOracle, "mm/Dd/yYYy hh:MI:SsAm")
			Convey("parses valid timestamps correctly", func() {
				value, err = p.Parse("01/04/2000 05:38:10PM")
				So(value.(time.Time), ShouldResemble, time.Date(2000, 1, 4, 17, 38, 10, 0, time.UTC))
				So(err, ShouldBeNil)
			})
			Convey("does not parse invalid dates", func() {
				_, err = p.Parse("01/04/2000 :) 05:38:10PM")
				So(err, ShouldNotBeNil)
				_, err = p.Parse("01/04/2000 005:38:10PM")
				So(err, ShouldNotBeNil)
				_, err = p.Parse("01/04/2000 5:38:10 PM")
				So(err, ShouldNotBeNil)
				_, err = p.Parse("01/04/2000")
				So(err, ShouldNotBeNil)
			})
		})
	})

	Convey("Using FieldDoubleParser", t, func() {
		var p, _ = NewFieldParser(ctDouble, "")
		var value interface{}
		var err error

		Convey("parses valid decimal values correctly", func() {
			value, err = p.Parse("3.14159265")
			So(value.(float64), ShouldEqual, 3.14159265)
			So(err, ShouldBeNil)
			value, err = p.Parse("0.123123")
			So(value.(float64), ShouldEqual, 0.123123)
			So(err, ShouldBeNil)
			value, err = p.Parse("-123456.789")
			So(value.(float64), ShouldEqual, -123456.789)
			So(err, ShouldBeNil)
			value, err = p.Parse("-1.")
			So(value.(float64), ShouldEqual, -1.0)
			So(err, ShouldBeNil)
		})
		Convey("does not parse invalid numbers", func() {
			_, err = p.Parse("")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("1.1.1")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("1-2.0")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("80-")
			So(err, ShouldNotBeNil)
		})
	})

	Convey("Using FieldInt32Parser", t, func() {
		var p, _ = NewFieldParser(ctInt32, "")
		var value interface{}
		var err error

		Convey("parses valid integer values correctly", func() {
			value, err = p.Parse("2147483647")
			So(value.(int32), ShouldEqual, 2147483647)
			So(err, ShouldBeNil)
			value, err = p.Parse("42")
			So(value.(int32), ShouldEqual, 42)
			So(err, ShouldBeNil)
			value, err = p.Parse("-2147483648")
			So(value.(int32), ShouldEqual, -2147483648)
		})
		Convey("does not parse invalid numbers", func() {
			_, err = p.Parse("")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("42.0")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("1-2")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("80-")
			So(err, ShouldNotBeNil)
			value, err = p.Parse("2147483648")
			So(err, ShouldNotBeNil)
			value, err = p.Parse("-2147483649")
			So(err, ShouldNotBeNil)
		})
	})

	Convey("Using FieldInt64Parser", t, func() {
		var p, _ = NewFieldParser(ctInt64, "")
		var value interface{}
		var err error

		Convey("parses valid integer values correctly", func() {
			value, err = p.Parse("2147483648")
			So(value.(int64), ShouldEqual, int64(2147483648))
			So(err, ShouldBeNil)
			value, err = p.Parse("42")
			So(value.(int64), ShouldEqual, 42)
			So(err, ShouldBeNil)
			value, err = p.Parse("-2147483649")
			So(value.(int64), ShouldEqual, int64(-2147483649))
		})
		Convey("does not parse invalid numbers", func() {
			_, err = p.Parse("")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("42.0")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("1-2")
			So(err, ShouldNotBeNil)
			_, err = p.Parse("80-")
			So(err, ShouldNotBeNil)
		})
	})

	Convey("Using FieldDecimalParser", t, func() {
		var p, _ = NewFieldParser(ctDecimal, "")
		var err error

		Convey("parses valid decimal values correctly", func() {
			for _, ts := range []string{"12235.2355", "42", "0", "-124", "-124.55"} {
				testVal, err := bson.ParseDecimal128(ts)
				So(err, ShouldBeNil)
				parsedValue, err := p.Parse(ts)
				So(err, ShouldBeNil)

				So(testVal, ShouldResemble, parsedValue.(bson.Decimal128))
			}
		})
		Convey("does not parse invalid decimal values", func() {
			for _, ts := range []string{"", "1-2", "abcd"} {
				_, err = p.Parse(ts)
				So(err, ShouldNotBeNil)
			}
		})
	})

	Convey("Using FieldStringParser", t, func() {
		var p, _ = NewFieldParser(ctString, "")
		var value interface{}
		var err error

		Convey("parses strings as strings only", func() {
			value, err = p.Parse("42")
			So(value.(string), ShouldEqual, "42")
			So(err, ShouldBeNil)
			value, err = p.Parse("true")
			So(value.(string), ShouldEqual, "true")
			So(err, ShouldBeNil)
			value, err = p.Parse("")
			So(value.(string), ShouldEqual, "")
			So(err, ShouldBeNil)
		})
	})

}
