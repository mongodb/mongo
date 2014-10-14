package mongoimport

import (
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"io"
	"io/ioutil"
	"os"
	"strings"
	"testing"
)

func init() {
	log.SetVerbosity(&options.Verbosity{
		Verbose: []bool{true, true, true, true},
	})
}

func TestValidateHeaders(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

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
			_, err = validateHeaders(NewCSVImportInput([]string{"a", "a.b.c"}, fileHandle), false)
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

func TestGetUpsertValue(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given a field and a BSON document, on calling getUpsertValue", t,
		func() {
			Convey("the value of the key should be correct for unnested "+
				"documents", func() {
				bsonDocument := bson.M{"a": 3}
				So(getUpsertValue("a", bsonDocument), ShouldEqual, 3)
			})
			Convey("the value of the key should be correct for nested "+
				"document fields", func() {
				bsonDocument := bson.M{"a": bson.M{"b": 4}}
				So(getUpsertValue("a.b", bsonDocument), ShouldEqual, 4)
			})
			Convey("the value of the key should be nil for unnested document "+
				"fields that do not exist", func() {
				bsonDocument := bson.M{"a": 4}
				So(getUpsertValue("c", bsonDocument), ShouldBeNil)
			})
			Convey("the value of the key should be nil for nested document "+
				"fields that do not exist", func() {
				bsonDocument := bson.M{"a": bson.M{"b": 4}}
				So(getUpsertValue("a.c", bsonDocument), ShouldBeNil)
			})
			Convey("the value of the key should be nil for nil document"+
				"values", func() {
				So(getUpsertValue("a", bson.M{"a": nil}), ShouldBeNil)
			})
		})
}

func TestConstructUpsertDocument(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given a set of upsert fields and a BSON document, on calling "+
		"constructUpsertDocument", t, func() {
		Convey("the key/value combination in the upsert document should be "+
			"correct for unnested documents with single fields", func() {
			bsonDocument := bson.M{"a": 3}
			upsertFields := []string{"a"}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldResemble, bsonDocument)
		})
		Convey("the key/value combination in the upsert document should be "+
			"correct for unnested documents with several fields", func() {
			bsonDocument := bson.M{"a": 3, "b": "string value"}
			upsertFields := []string{"a"}
			expectedDocument := bson.M{"a": 3}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldResemble, expectedDocument)
		})
		Convey("the key/value combination in the upsert document should be "+
			"correct for nested documents with several fields", func() {
			bsonDocument := bson.M{"a": bson.M{testCollection: 4}, "b": "string value"}
			upsertFields := []string{"a.c"}
			expectedDocument := bson.M{"a.c": 4}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldResemble, expectedDocument)
		})
		Convey("the upsert document should be nil if the key does not exist "+
			"in the BSON document", func() {
			bsonDocument := bson.M{"a": 3, "b": "string value"}
			upsertFields := []string{testCollection}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldBeNil)
		})
	})
}

func TestGetParsedValue(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

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
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

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
