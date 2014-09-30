package mongoimport

import (
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"io"
	"io/ioutil"
	"os"
	"testing"
)

func TestJSONArrayImportDocument(t *testing.T) {
	Convey("With a JSON array import input", t, func() {
		var err error
		var jsonFile, fileHandle *os.File
		Convey("an error should be thrown if a plain JSON document is supplied",
			func() {
				contents := `{"a": "ae"}`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				_, err = NewJSONImportInput(true, fileHandle).ImportDocument()
				So(err, ShouldNotBeNil)
			})

		Convey("reading a JSON object that has no opening bracket should "+
			"error out",
			func() {
				contents := `{"a":3},{"b":4}]`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				_, err = NewJSONImportInput(true, fileHandle).ImportDocument()
				So(err, ShouldNotBeNil)
			})

		Convey("JSON arrays that do not end with a closing bracket should "+
			"error out",
			func() {
				contents := `[{"a": "ae"}`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				_, err = NewJSONImportInput(true, fileHandle).ImportDocument()
				So(err, ShouldBeNil)
				_, err = NewJSONImportInput(true, fileHandle).ImportDocument()
				So(err, ShouldNotBeNil)
			})

		// TODO: we'll accept inputs like [[{},{}]] and just do nothing instead
		// of alerting the user of an error
		Convey("an error should be thrown if a plain JSON file is supplied",
			func() {
				fileHandle, err := os.Open("testdata/test_plain.json")
				So(err, ShouldBeNil)
				_, err = NewJSONImportInput(true, fileHandle).ImportDocument()
				So(err, ShouldNotBeNil)
			})

		Convey("array JSON input file sources should be parsed correctly and "+
			"subsequent imports should parse correctly",
			func() {
				// TODO: currently parses JSON as floats and not ints
				expectedReadOne := bson.M{"a": 1.2, "b": "a", "c": 0.4}
				expectedReadTwo := bson.M{"a": 2.4, "b": "string", "c": 52.9}
				fileHandle, err := os.Open("testdata/test_array.json")
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(true, fileHandle)
				bsonDoc, err := jsonImporter.ImportDocument()
				So(err, ShouldBeNil)
				So(bsonDoc, ShouldResemble, expectedReadOne)
				bsonDoc, err = jsonImporter.ImportDocument()
				So(err, ShouldBeNil)
				So(bsonDoc, ShouldResemble, expectedReadTwo)
			})

		Reset(func() {
			jsonFile.Close()
			fileHandle.Close()
		})
	})
}

func TestJSONPlainImportDocument(t *testing.T) {
	Convey("With a plain JSON import input", t, func() {
		var err error
		var jsonFile, fileHandle *os.File
		Convey("string valued JSON documents should be imported properly",
			func() {
				contents := `{"a": "ae"}`
				expectedRead := bson.M{"a": "ae"}
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(false, fileHandle)
				bsonDoc, err := jsonImporter.ImportDocument()
				So(err, ShouldBeNil)
				So(bsonDoc, ShouldResemble, expectedRead)
			})

		Convey("several string valued JSON documents should be imported "+
			"properly", func() {
			contents := `{"a": "ae"}{"b": "dc"}`
			expectedReadOne := bson.M{"a": "ae"}
			expectedReadTwo := bson.M{"b": "dc"}
			jsonFile, err = ioutil.TempFile("", "mongoimport_")
			So(err, ShouldBeNil)
			_, err = io.WriteString(jsonFile, contents)
			So(err, ShouldBeNil)
			fileHandle, err := os.Open(jsonFile.Name())
			So(err, ShouldBeNil)
			jsonImporter := NewJSONImportInput(false, fileHandle)
			bsonDoc, err := jsonImporter.ImportDocument()
			So(err, ShouldBeNil)
			So(bsonDoc, ShouldResemble, expectedReadOne)
			bsonDoc, err = jsonImporter.ImportDocument()
			So(err, ShouldBeNil)
			So(bsonDoc, ShouldResemble, expectedReadTwo)
		})

		Convey("number valued JSON documents should be imported properly",
			func() {
				contents := `{"a": "ae", "b": 2.0}`
				expectedRead := bson.M{"a": "ae", "b": 2.0}
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(false, fileHandle)
				bsonDoc, err := jsonImporter.ImportDocument()
				So(err, ShouldBeNil)
				So(bsonDoc, ShouldResemble, expectedRead)
			})

		Convey("JSON arrays should return an error", func() {
			contents := `[{"a": "ae", "b": 2.0}]`
			jsonFile, err = ioutil.TempFile("", "mongoimport_")
			So(err, ShouldBeNil)
			_, err = io.WriteString(jsonFile, contents)
			So(err, ShouldBeNil)
			fileHandle, err := os.Open(jsonFile.Name())
			So(err, ShouldBeNil)
			jsonImporter := NewJSONImportInput(false, fileHandle)
			bsonDoc, err := jsonImporter.ImportDocument()
			So(err, ShouldNotBeNil)
			So(bsonDoc, ShouldBeNil)
		})

		Convey("plain JSON input file sources should be parsed correctly and "+
			"subsequent imports should parse correctly",
			func() {
				expectedReadOne := bson.M{"a": 4, "b": "string value", "c": 1}
				expectedReadTwo := bson.M{"a": 5, "b": "string value", "c": 2}
				fileHandle, err := os.Open("testdata/test_plain.json")
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(false, fileHandle)
				bsonDoc, err := jsonImporter.ImportDocument()
				So(err, ShouldBeNil)
				So(bsonDoc, ShouldNotResemble, expectedReadOne)
				bsonDoc, err = jsonImporter.ImportDocument()
				So(err, ShouldBeNil)
				So(bsonDoc, ShouldNotResemble, expectedReadTwo)
			})

		Reset(func() {
			jsonFile.Close()
			fileHandle.Close()
		})
	})
}

func TestReadJSONArraySeparator(t *testing.T) {
	Convey("With an array JSON import input", t, func() {
		var err error
		var jsonFile, fileHandle *os.File
		Convey("reading a JSON array separator should consume [",
			func() {
				contents := `[{"a": "ae"}`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(true, fileHandle)
				So(jsonImporter.readJSONArraySeparator(), ShouldBeNil)
				// at this point it should have consumed all bytes up to `{`
				So(jsonImporter.readJSONArraySeparator(), ShouldNotBeNil)
			})
		Convey("reading a closing JSON array separator without a "+
			"corresponding opening bracket should error out ",
			func() {
				contents := `]`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(true, fileHandle)
				So(jsonImporter.readJSONArraySeparator(), ShouldNotBeNil)
			})
		Convey("reading an opening JSON array separator without a "+
			"corresponding closing bracket should error out ",
			func() {
				contents := `[`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(true, fileHandle)
				So(jsonImporter.readJSONArraySeparator(), ShouldBeNil)
				So(jsonImporter.readJSONArraySeparator(), ShouldNotBeNil)
			})
		Convey("reading an opening JSON array separator with an ending "+
			"closing bracket should return EOF",
			func() {
				contents := `[]`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(true, fileHandle)
				So(jsonImporter.readJSONArraySeparator(), ShouldBeNil)
				So(jsonImporter.readJSONArraySeparator(), ShouldEqual, io.EOF)
			})
		Convey("reading an opening JSON array separator, an ending closing "+
			"bracket but then additional characters after that, should error",
			func() {
				contents := `[]a`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(true, fileHandle)
				So(jsonImporter.readJSONArraySeparator(), ShouldBeNil)
				So(jsonImporter.readJSONArraySeparator(), ShouldNotBeNil)
			})
		Convey("reading invalid JSON objects between valid objects should "+
			"error out",
			func() {
				contents := `[{"a":3}x{"b":4}]`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(true, fileHandle)
				_, err = jsonImporter.ImportDocument()
				So(err, ShouldBeNil)
				So(jsonImporter.readJSONArraySeparator(), ShouldNotBeNil)
			})
		Convey("reading invalid JSON objects after valid objects but between "+
			"valid objects should error out",
			func() {
				contents := `[{"a":3},b{"b":4}]`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err := os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				jsonImporter := NewJSONImportInput(true, fileHandle)
				_, err = jsonImporter.ImportDocument()
				So(err, ShouldBeNil)
				So(jsonImporter.readJSONArraySeparator(), ShouldBeNil)
				So(jsonImporter.readJSONArraySeparator(), ShouldNotBeNil)

				contents = `[{"a":3},,{"b":4}]`
				jsonFile, err = ioutil.TempFile("", "mongoimport_")
				So(err, ShouldBeNil)
				_, err = io.WriteString(jsonFile, contents)
				So(err, ShouldBeNil)
				fileHandle, err = os.Open(jsonFile.Name())
				So(err, ShouldBeNil)
				jsonImporter = NewJSONImportInput(true, fileHandle)
				_, err = jsonImporter.ImportDocument()
				So(err, ShouldBeNil)
				_, err = jsonImporter.ImportDocument()
				So(err, ShouldBeNil)
			})
		Reset(func() {
			jsonFile.Close()
			fileHandle.Close()
		})
	})
}
