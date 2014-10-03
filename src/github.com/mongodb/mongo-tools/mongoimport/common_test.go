package mongoimport

import (
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"io"
	"io/ioutil"
	"os"
	"strings"
	"testing"
)

func init() {
	log.InitToolLogger(&options.Verbosity{
		Verbose: []bool{true, true, true, true},
	})
}

func TestValidateHeaders(t *testing.T) {
	// validateHeaders
	Convey("Given an import input, in validating the headers", t, func() {
		fields := []string{"a", "b", "c"}
		contents := "headerLine1, headerLine2"
		csvFile, err := ioutil.TempFile("", "mongoimport_")
		So(err, ShouldBeNil)
		_, err = io.WriteString(csvFile, contents)
		So(err, ShouldBeNil)
		fileHandle, err := os.Open(csvFile.Name())
		So(err, ShouldBeNil)

		Convey("if headerLine is true, the first line in the input should be used", func() {
			headers, err := validateHeaders(NewCSVImportInput(fields, fileHandle), true)
			So(err, ShouldBeNil)
			So(len(headers), ShouldEqual, 2)
			// spaces are trimed in the header
			So(headers, ShouldResemble, strings.Split(strings.Replace(contents, " ", "", -1), ","))
		})
		Convey("if headerLine is false, the fields passed in should be used", func() {
			headers, err := validateHeaders(NewCSVImportInput(fields, fileHandle), false)
			So(err, ShouldBeNil)
			So(len(headers), ShouldEqual, 3)
			// spaces are trimed in the header
			So(headers, ShouldResemble, fields)
		})
		Convey("if the fields contain '..', an error should be thrown", func() {
			_, err := validateHeaders(NewCSVImportInput([]string{"a..a"}, fileHandle), false)
			So(err, ShouldNotBeNil)
		})
		Convey("if the fields start/end in a '.', an error should be thrown", func() {
			_, err := validateHeaders(NewCSVImportInput([]string{".a"}, fileHandle), false)
			So(err, ShouldNotBeNil)
			_, err = validateHeaders(NewCSVImportInput([]string{"a."}, fileHandle), false)
			So(err, ShouldNotBeNil)
		})
		Convey("if the fields collide, an error should be thrown", func() {
			_, err := validateHeaders(NewCSVImportInput([]string{"a", "a.a"}, fileHandle), false)
			So(err, ShouldNotBeNil)
			_, err = validateHeaders(NewCSVImportInput([]string{"a", "a.ba", "b.a"}, fileHandle), false)
			So(err, ShouldNotBeNil)
		})
		Convey("if the fields don't collide, no error should be thrown", func() {
			_, err := validateHeaders(NewCSVImportInput([]string{"a", "aa"}, fileHandle), false)
			So(err, ShouldBeNil)
			_, err = validateHeaders(NewCSVImportInput([]string{"a", "aa", "b.a", "b.c"}, fileHandle), false)
			So(err, ShouldBeNil)
			_, err = validateHeaders(NewCSVImportInput([]string{"a", "ba", "ab", "b.a"}, fileHandle), false)
			So(err, ShouldBeNil)
			_, err = validateHeaders(NewCSVImportInput([]string{"a", "ba", "ab", "b.a", "b.c.d"}, fileHandle), false)
			So(err, ShouldBeNil)
			_, err = validateHeaders(NewCSVImportInput([]string{"a", "ab.c"}, fileHandle), false)
			So(err, ShouldBeNil)
		})
		Convey("if the fields contain the same keys, an error should be thrown", func() {
			_, err := validateHeaders(NewCSVImportInput([]string{"a", "ba", "a"}, fileHandle), false)
			So(err, ShouldNotBeNil)
		})
	})
}

func TestGetParsedValue(t *testing.T) {
	Convey("Given a string token to parse", t, func() {
		Convey("an int token should return the underlying int value", func() {
			So(getParsedValue("3"), ShouldEqual, 3)
		})
		Convey("a float token should return the underlying float value", func() {
			So(getParsedValue(".33"), ShouldEqual, 0.33)
		})
		Convey("a string token should return the underlying string value", func() {
			So(getParsedValue("sd"), ShouldEqual, "sd")
		})
	})
}

func TestSetNestedValue(t *testing.T) {
	Convey("Given a field, its value, and an existing BSON document...", t, func() {
		currentDocument := bson.M{
			"a": 3,
			"b": bson.M{
				"c": "d",
			},
		}
		Convey("ensure top level fields are set and others, unchanged", func() {
			testDocument := currentDocument
			expectedDocument := bson.M{
				"a": 3,
				"b": bson.M{
					"c": "d",
				},
				"c": 4,
			}
			setNestedValue("c", 4, testDocument)
			So(testDocument, ShouldResemble, expectedDocument)
		})
		Convey("ensure new nested top-level fields are set and others, unchanged", func() {
			testDocument := currentDocument
			expectedDocument := bson.M{
				"a": 3,
				"b": bson.M{
					"c": "d",
				},
				"c": bson.M{
					"b": "4",
				},
			}
			setNestedValue("c.b", "4", testDocument)
			So(testDocument, ShouldResemble, expectedDocument)
		})
		Convey("ensure existing nested level fields are set and others, unchanged", func() {
			testDocument := currentDocument
			expectedDocument := bson.M{
				"a": 3,
				"b": bson.M{
					"c": "d",
					"d": 9,
				},
			}
			setNestedValue("b.d", 9, testDocument)
			So(testDocument, ShouldResemble, expectedDocument)
		})
		Convey("ensure subsequent calls update fields accordingly", func() {
			testDocument := currentDocument
			expectedDocumentOne := bson.M{
				"a": 3,
				"b": bson.M{
					"c": "d",
					"d": 9,
				},
			}
			expectedDocumentTwo := bson.M{
				"a": 3,
				"b": bson.M{
					"c": "d",
					"d": 9,
				},
				"f": 23,
			}
			setNestedValue("b.d", 9, testDocument)
			So(testDocument, ShouldResemble, expectedDocumentOne)
			setNestedValue("f", 23, testDocument)
			So(testDocument, ShouldResemble, expectedDocumentTwo)
		})
	})
}
