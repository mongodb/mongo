package mongoimport

import (
	"bytes"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"strings"
	"testing"
)

func init() {
	log.SetVerbosity(&options.Verbosity{
		Verbose: []bool{true, true, true, true},
	})
}

func TestCSVImportDocument(t *testing.T) {
	testutil.VerifyTestType(t, "unit")

	Convey("With a CSV import input", t, func() {
		var err error
		Convey("badly encoded csv should result in a parsing error", func() {
			contents := `1, 2, foo"bar`
			fields := []string{"a", "b", "c"}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			_, err = csvImporter.ImportDocument()
			So(err, ShouldNotBeNil)
		})
		Convey("escaped quotes are parsed correctly", func() {
			contents := `1, 2, "foo""bar"`
			fields := []string{"a", "b", "c"}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			_, err = csvImporter.ImportDocument()
			So(err, ShouldBeNil)
		})
		Convey("whitespace separated quoted strings are still an error", func() {
			contents := `1, 2, "foo"  "bar"`
			fields := []string{"a", "b", "c"}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			_, err = csvImporter.ImportDocument()
			So(err, ShouldNotBeNil)
		})
		Convey("multiple escaped quotes separated by whitespace parsed correctly", func() {
			contents := `1, 2, "foo"" ""bar"`
			fields := []string{"a", "b", "c"}
			expectedRead := bson.M{
				"a": 1,
				"b": 2,
				"c": `foo" "bar`,
			}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			bsonDoc, err := csvImporter.ImportDocument()
			So(err, ShouldBeNil)
			So(bsonDoc, ShouldResemble, expectedRead)
		})
		Convey("integer valued strings should be converted", func() {
			contents := `1, 2, " 3e"`
			fields := []string{"a", "b", "c"}
			expectedRead := bson.M{
				"a": 1,
				"b": 2,
				"c": " 3e",
			}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			bsonDoc, err := csvImporter.ImportDocument()
			So(err, ShouldBeNil)
			So(bsonDoc, ShouldResemble, expectedRead)
		})

		Convey("extra fields should be prefixed with 'field'", func() {
			contents := `1, 2f , " 3e" , " may"`
			fields := []string{"a", "b", "c"}
			expectedRead := bson.M{
				"a":      1,
				"b":      "2f",
				"c":      " 3e",
				"field3": " may",
			}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			bsonDoc, err := csvImporter.ImportDocument()
			So(err, ShouldBeNil)
			So(bsonDoc, ShouldResemble, expectedRead)
		})

		Convey("nested csv fields should be imported properly", func() {
			contents := `1, 2f , " 3e" , " may"`
			fields := []string{"a", "b.c", "c"}
			expectedRead := bson.M{
				"a": 1,
				"b": bson.M{
					"c": "2f",
				},
				"c":      " 3e",
				"field3": " may",
			}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			bsonDoc, err := csvImporter.ImportDocument()
			So(err, ShouldBeNil)
			So(bsonDoc, ShouldResemble, expectedRead)
		})

		Convey("nested csv fields causing header collisions should error", func() {
			contents := `1, 2f , " 3e" , " may", june`
			fields := []string{"a", "b.c", "field3"}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			_, err := csvImporter.ImportDocument()
			So(err, ShouldNotBeNil)
		})

		Convey("calling ImportDocument() for CSVs should return next set of "+
			"values", func() {
			contents := "1, 2, 3\n4, 5, 6"
			fields := []string{"a", "b", "c"}
			expectedReadOne := bson.M{
				"a": 1,
				"b": 2,
				"c": 3,
			}
			expectedReadTwo := bson.M{
				"a": 4,
				"b": 5,
				"c": 6,
			}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			bsonDoc, err := csvImporter.ImportDocument()
			So(err, ShouldBeNil)
			So(bsonDoc, ShouldResemble, expectedReadOne)
			bsonDoc, err = csvImporter.ImportDocument()
			So(err, ShouldBeNil)
			So(bsonDoc, ShouldResemble, expectedReadTwo)
		})
	})
}

func TestCSVSetHeader(t *testing.T) {
	testutil.VerifyTestType(t, "unit")

	var err error
	Convey("With a CSV import input", t, func() {
		Convey("setting the header should read the first line of the CSV",
			func() {
				contents := "extraHeader1, extraHeader2, extraHeader3"
				fields := []string{}
				csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
				So(csvImporter.SetHeader(true), ShouldBeNil)
				So(len(csvImporter.Fields), ShouldEqual, 3)
			})

		Convey("setting non-colliding nested csv headers should not raise an error", func() {
			contents := "a, b, c"
			fields := []string{}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			So(csvImporter.SetHeader(true), ShouldBeNil)
			So(len(csvImporter.Fields), ShouldEqual, 3)
			contents = "a.b.c, a.b.d, c"
			fields = []string{}
			csvImporter = NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			So(csvImporter.SetHeader(true), ShouldBeNil)
			So(len(csvImporter.Fields), ShouldEqual, 3)

			contents = "a.b, ab, a.c"
			fields = []string{}
			csvImporter = NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			So(csvImporter.SetHeader(true), ShouldBeNil)
			So(len(csvImporter.Fields), ShouldEqual, 3)

			contents = "a, ab, ac, dd"
			fields = []string{}
			csvImporter = NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			So(csvImporter.SetHeader(true), ShouldBeNil)
			So(len(csvImporter.Fields), ShouldEqual, 4)
		})

		Convey("setting colliding nested csv headers should raise an error", func() {
			contents := "a, a.b, c"
			fields := []string{}
			csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			So(csvImporter.SetHeader(true), ShouldNotBeNil)

			contents = "a.b.c, a.b.d.c, a.b.d"
			fields = []string{}
			csvImporter = NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			So(csvImporter.SetHeader(true), ShouldNotBeNil)

			contents = "a, a, a"
			fields = []string{}
			csvImporter = NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
			So(csvImporter.SetHeader(true), ShouldNotBeNil)
		})

		Convey("setting the header that ends in a dot should error",
			func() {
				contents := "c, a., b"
				fields := []string{}
				So(err, ShouldBeNil)
				So(NewCSVImportInput(fields, bytes.NewReader([]byte(contents))).SetHeader(true), ShouldNotBeNil)
			})

		Convey("setting the header that starts in a dot should error",
			func() {
				contents := "c, .a, b"
				fields := []string{}
				So(NewCSVImportInput(fields, bytes.NewReader([]byte(contents))).SetHeader(true), ShouldNotBeNil)
			})

		Convey("setting the header that contains multiple consecutive dots should error",
			func() {
				contents := "c, a..a, b"
				fields := []string{}
				So(NewCSVImportInput(fields, bytes.NewReader([]byte(contents))).SetHeader(true), ShouldNotBeNil)

				contents = "c, a.a, b.b...b"
				fields = []string{}
				So(NewCSVImportInput(fields, bytes.NewReader([]byte(contents))).SetHeader(true), ShouldNotBeNil)
			})

		Convey("setting the header using an empty file should return EOF",
			func() {
				contents := ""
				fields := []string{}
				csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
				So(csvImporter.SetHeader(true), ShouldEqual, io.EOF)
				So(len(csvImporter.Fields), ShouldEqual, 0)
			})
		Convey("setting the header with fields already set, should "+
			"the header line with the existing fields",
			func() {
				contents := "extraHeader1,extraHeader2,extraHeader3"
				fields := []string{"a", "b", "c"}
				csvImporter := NewCSVImportInput(fields, bytes.NewReader([]byte(contents)))
				So(csvImporter.SetHeader(true), ShouldBeNil)
				// if SetHeader() is called with fields already passed in,
				// the header should be replaced with the read header line
				So(len(csvImporter.Fields), ShouldEqual, 3)
				So(csvImporter.Fields, ShouldResemble, strings.Split(contents, ","))
			})

		Convey("plain CSV input file sources should be parsed correctly and "+
			"subsequent imports should parse correctly",
			func() {
				fields := []string{"a", "b", "c"}
				expectedReadOne := bson.M{"a": 1, "b": 2, "c": 3}
				expectedReadTwo := bson.M{"a": 3, "b": 5.4, "c": "string"}
				fileHandle, err := os.Open("testdata/test.csv")
				So(err, ShouldBeNil)
				csvImporter := NewCSVImportInput(fields, fileHandle)
				bsonDoc, err := csvImporter.ImportDocument()
				So(err, ShouldBeNil)
				So(bsonDoc, ShouldResemble, expectedReadOne)
				bsonDoc, err = csvImporter.ImportDocument()
				So(err, ShouldBeNil)
				So(bsonDoc, ShouldResemble, expectedReadTwo)
			})
	})
}
