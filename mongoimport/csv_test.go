package mongoimport

import (
	"bytes"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
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

func TestCSVStreamDocument(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)
	Convey("With a CSV input reader", t, func() {
		Convey("badly encoded CSV should result in a parsing error", func() {
			contents := `1, 2, foo"bar`
			fields := []string{"a", "b", "c"}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go csvInputReader.StreamDocument(true, docChan, errChan)
			So(<-errChan, ShouldNotBeNil)
		})
		Convey("escaped quotes are parsed correctly", func() {
			contents := `1, 2, "foo""bar"`
			fields := []string{"a", "b", "c"}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go csvInputReader.StreamDocument(true, docChan, errChan)
			So(<-errChan, ShouldBeNil)
		})
		Convey("multiple escaped quotes separated by whitespace parsed correctly", func() {
			contents := `1, 2, "foo"" ""bar"`
			fields := []string{"a", "b", "c"}
			expectedRead := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", `foo" "bar`},
			}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go csvInputReader.StreamDocument(true, docChan, errChan)
			So(<-errChan, ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})
		Convey("integer valued strings should be converted", func() {
			contents := `1, 2, " 3e"`
			fields := []string{"a", "b", "c"}
			expectedRead := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", " 3e"},
			}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go csvInputReader.StreamDocument(true, docChan, errChan)
			So(<-errChan, ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
		})
		Convey("extra fields should be prefixed with 'field'", func() {
			contents := `1, 2f , " 3e" , " may"`
			fields := []string{"a", "b", "c"}
			expectedRead := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", "2f"},
				bson.DocElem{"c", " 3e"},
				bson.DocElem{"field3", " may"},
			}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go csvInputReader.StreamDocument(true, docChan, errChan)
			So(<-docChan, ShouldResemble, expectedRead)
			So(<-errChan, ShouldBeNil)
		})
		Convey("nested CSV fields should be imported properly", func() {
			contents := `1, 2f , " 3e" , " may"`
			fields := []string{"a", "b.c", "c"}
			expectedRead := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", bson.D{
					bson.DocElem{"c", "2f"},
				}},
				bson.DocElem{"c", " 3e"},
				bson.DocElem{"field3", " may"},
			}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go csvInputReader.StreamDocument(true, docChan, errChan)
			readDocument := <-docChan
			So(readDocument[0], ShouldResemble, expectedRead[0])
			So(readDocument[1].Name, ShouldResemble, expectedRead[1].Name)
			So(*readDocument[1].Value.(*bson.D), ShouldResemble, expectedRead[1].Value)
			So(readDocument[2], ShouldResemble, expectedRead[2])
			So(readDocument[3], ShouldResemble, expectedRead[3])
			So(<-errChan, ShouldBeNil)
		})
		Convey("whitespace separated quoted strings are still an error", func() {
			contents := `1, 2, "foo"  "bar"`
			fields := []string{"a", "b", "c"}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go csvInputReader.StreamDocument(true, docChan, errChan)
			So(<-errChan, ShouldNotBeNil)
		})
		Convey("nested CSV fields causing header collisions should error", func() {
			contents := `1, 2f , " 3e" , " may", june`
			fields := []string{"a", "b.c", "field3"}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go csvInputReader.StreamDocument(true, docChan, errChan)
			So(<-errChan, ShouldNotBeNil)
		})
		Convey("calling StreamDocument() for CSVs should return next set of "+
			"values", func() {
			contents := "1, 2, 3\n4, 5, 6"
			fields := []string{"a", "b", "c"}
			expectedReadOne := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", 3},
			}
			expectedReadTwo := bson.D{
				bson.DocElem{"a", 4},
				bson.DocElem{"b", 5},
				bson.DocElem{"c", 6},
			}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go csvInputReader.StreamDocument(true, docChan, errChan)
			So(<-docChan, ShouldResemble, expectedReadOne)
			So(<-docChan, ShouldResemble, expectedReadTwo)
			So(<-errChan, ShouldBeNil)
		})
	})
}

func TestCSVSetFields(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)
	var err error
	Convey("With a CSV input reader", t, func() {
		Convey("setting the header should read the first line of the CSV", func() {
			contents := "extraHeader1, extraHeader2, extraHeader3"
			fields := []string{}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(csvInputReader.SetFields(true), ShouldBeNil)
			So(len(csvInputReader.Fields), ShouldEqual, 3)
		})

		Convey("setting non-colliding nested CSV headers should not raise an error", func() {
			contents := "a, b, c"
			fields := []string{}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(csvInputReader.SetFields(true), ShouldBeNil)
			So(len(csvInputReader.Fields), ShouldEqual, 3)
			contents = "a.b.c, a.b.d, c"
			fields = []string{}
			csvInputReader = NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(csvInputReader.SetFields(true), ShouldBeNil)
			So(len(csvInputReader.Fields), ShouldEqual, 3)

			contents = "a.b, ab, a.c"
			fields = []string{}
			csvInputReader = NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(csvInputReader.SetFields(true), ShouldBeNil)
			So(len(csvInputReader.Fields), ShouldEqual, 3)

			contents = "a, ab, ac, dd"
			fields = []string{}
			csvInputReader = NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(csvInputReader.SetFields(true), ShouldBeNil)
			So(len(csvInputReader.Fields), ShouldEqual, 4)
		})

		Convey("setting colliding nested CSV headers should raise an error", func() {
			contents := "a, a.b, c"
			fields := []string{}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(csvInputReader.SetFields(true), ShouldNotBeNil)

			contents = "a.b.c, a.b.d.c, a.b.d"
			fields = []string{}
			csvInputReader = NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(csvInputReader.SetFields(true), ShouldNotBeNil)

			contents = "a, a, a"
			fields = []string{}
			csvInputReader = NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(csvInputReader.SetFields(true), ShouldNotBeNil)
		})

		Convey("setting the header that ends in a dot should error", func() {
			contents := "c, a., b"
			fields := []string{}
			So(err, ShouldBeNil)
			So(NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1).SetFields(true), ShouldNotBeNil)
		})

		Convey("setting the header that starts in a dot should error", func() {
			contents := "c, .a, b"
			fields := []string{}
			So(NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1).SetFields(true), ShouldNotBeNil)
		})

		Convey("setting the header that contains multiple consecutive dots should error", func() {
			contents := "c, a..a, b"
			fields := []string{}
			So(NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1).SetFields(true), ShouldNotBeNil)

			contents = "c, a.a, b.b...b"
			fields = []string{}
			So(NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1).SetFields(true), ShouldNotBeNil)
		})

		Convey("setting the header using an empty file should return EOF", func() {
			contents := ""
			fields := []string{}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(csvInputReader.SetFields(true), ShouldEqual, io.EOF)
			So(len(csvInputReader.Fields), ShouldEqual, 0)
		})
		Convey("setting the header with fields already set, should "+
			"the header line with the existing fields", func() {
			contents := "extraHeader1,extraHeader2,extraHeader3"
			fields := []string{"a", "b", "c"}
			csvInputReader := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(csvInputReader.SetFields(true), ShouldBeNil)
			// if SetFields() is called with fields already passed in,
			// the header should be replaced with the read header line
			So(len(csvInputReader.Fields), ShouldEqual, 3)
			So(csvInputReader.Fields, ShouldResemble, strings.Split(contents, ","))
		})
		Convey("plain CSV input file sources should be parsed correctly and "+
			"subsequent imports should parse correctly", func() {
			fields := []string{"a", "b", "c"}
			expectedReadOne := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", 3},
			}
			expectedReadTwo := bson.D{
				bson.DocElem{"a", 3},
				bson.DocElem{"b", 5.4},
				bson.DocElem{"c", "string"},
			}
			fileHandle, err := os.Open("testdata/test.csv")
			So(err, ShouldBeNil)
			csvInputReader := NewCSVInputReader(fields, fileHandle, 1)

			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go csvInputReader.StreamDocument(true, docChan, errChan)
			So(<-docChan, ShouldResemble, expectedReadOne)
			So(<-docChan, ShouldResemble, expectedReadTwo)
			So(<-errChan, ShouldBeNil)
		})
	})
}

func TestCSVReadHeaderFromSource(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)
	Convey("With a CSV input reader", t, func() {
		Convey("getting the header should return any already set headers", func() {
			expectedHeaders := []string{"1", "2", "3"}
			fileHandle, err := os.Open("testdata/test.csv")
			So(err, ShouldBeNil)
			csvInputReader := NewCSVInputReader([]string{}, fileHandle, 1)
			headers, err := csvInputReader.ReadHeaderFromSource()
			So(err, ShouldBeNil)
			So(headers, ShouldResemble, expectedHeaders)
		})
	})
}

func TestCSVConvert(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)
	Convey("With a CSV input reader", t, func() {
		Convey("calling convert on a CSVConvertibleDoc should return the expected BSON document", func() {
			numProcessed := uint64(0)
			csvConvertibleDoc := CSVConvertibleDoc{
				fields:       []string{"field1", "field2", "field3"},
				data:         []string{"a", "b", "c"},
				numProcessed: &numProcessed,
			}
			expectedDocument := bson.D{
				bson.DocElem{"field1", "a"},
				bson.DocElem{"field2", "b"},
				bson.DocElem{"field3", "c"},
			}
			document, err := csvConvertibleDoc.Convert()
			So(err, ShouldBeNil)
			So(document, ShouldResemble, expectedDocument)
		})
	})
}
