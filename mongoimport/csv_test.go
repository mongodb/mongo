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
		VLevel: 4,
	})
}

func TestCSVStreamDocument(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("With a CSV input reader", t, func() {
		Convey("badly encoded CSV should result in a parsing error", func() {
			contents := `1, 2, foo"bar`
			fields := []string{"a", "b", "c"}
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldNotBeNil)
		})
		Convey("escaped quotes are parsed correctly", func() {
			contents := `1, 2, "foo""bar"`
			fields := []string{"a", "b", "c"}
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
		})
		Convey("multiple escaped quotes separated by whitespace parsed correctly", func() {
			contents := `1, 2, "foo"" ""bar"`
			fields := []string{"a", "b", "c"}
			expectedRead := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", `foo" "bar`},
			}
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
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
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
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
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedRead)
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
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 4)
			So(r.StreamDocument(true, docChan), ShouldBeNil)

			readDocument := <-docChan
			So(readDocument[0], ShouldResemble, expectedRead[0])
			So(readDocument[1].Name, ShouldResemble, expectedRead[1].Name)
			So(*readDocument[1].Value.(*bson.D), ShouldResemble, expectedRead[1].Value)
			So(readDocument[2], ShouldResemble, expectedRead[2])
			So(readDocument[3], ShouldResemble, expectedRead[3])
		})
		Convey("whitespace separated quoted strings are still an error", func() {
			contents := `1, 2, "foo"  "bar"`
			fields := []string{"a", "b", "c"}
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldNotBeNil)
		})
		Convey("nested CSV fields causing header collisions should error", func() {
			contents := `1, 2f , " 3e" , " may", june`
			fields := []string{"a", "b.c", "field3"}
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 1)
			So(r.StreamDocument(true, docChan), ShouldNotBeNil)
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
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			docChan := make(chan bson.D, 2)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedReadOne)
			So(<-docChan, ShouldResemble, expectedReadTwo)
		})
	})
}

func TestCSVReadAndValidateHeader(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	var err error
	Convey("With a CSV input reader", t, func() {
		Convey("setting the header should read the first line of the CSV", func() {
			contents := "extraHeader1, extraHeader2, extraHeader3"
			fields := []string{}
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.fields), ShouldEqual, 3)
		})

		Convey("setting non-colliding nested CSV headers should not raise an error", func() {
			contents := "a, b, c"
			fields := []string{}
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.fields), ShouldEqual, 3)
			contents = "a.b.c, a.b.d, c"
			fields = []string{}
			r = NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.fields), ShouldEqual, 3)

			contents = "a.b, ab, a.c"
			fields = []string{}
			r = NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.fields), ShouldEqual, 3)

			contents = "a, ab, ac, dd"
			fields = []string{}
			r = NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			So(len(r.fields), ShouldEqual, 4)
		})

		Convey("setting colliding nested CSV headers should raise an error", func() {
			contents := "a, a.b, c"
			fields := []string{}
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldNotBeNil)

			contents = "a.b.c, a.b.d.c, a.b.d"
			fields = []string{}
			r = NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldNotBeNil)

			contents = "a, a, a"
			fields = []string{}
			r = NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldNotBeNil)
		})

		Convey("setting the header that ends in a dot should error", func() {
			contents := "c, a., b"
			fields := []string{}
			So(err, ShouldBeNil)
			So(NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1).ReadAndValidateHeader(), ShouldNotBeNil)
		})

		Convey("setting the header that starts in a dot should error", func() {
			contents := "c, .a, b"
			fields := []string{}
			So(NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1).ReadAndValidateHeader(), ShouldNotBeNil)
		})

		Convey("setting the header that contains multiple consecutive dots should error", func() {
			contents := "c, a..a, b"
			fields := []string{}
			So(NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1).ReadAndValidateHeader(), ShouldNotBeNil)

			contents = "c, a.a, b.b...b"
			fields = []string{}
			So(NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1).ReadAndValidateHeader(), ShouldNotBeNil)
		})

		Convey("setting the header using an empty file should return EOF", func() {
			contents := ""
			fields := []string{}
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldEqual, io.EOF)
			So(len(r.fields), ShouldEqual, 0)
		})
		Convey("setting the header with fields already set, should "+
			"the header line with the existing fields", func() {
			contents := "extraHeader1,extraHeader2,extraHeader3"
			fields := []string{"a", "b", "c"}
			r := NewCSVInputReader(fields, bytes.NewReader([]byte(contents)), 1)
			So(r.ReadAndValidateHeader(), ShouldBeNil)
			// if ReadAndValidateHeader() is called with fields already passed in,
			// the header should be replaced with the read header line
			So(len(r.fields), ShouldEqual, 3)
			So(r.fields, ShouldResemble, strings.Split(contents, ","))
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
			r := NewCSVInputReader(fields, fileHandle, 1)
			docChan := make(chan bson.D, 50)
			So(r.StreamDocument(true, docChan), ShouldBeNil)
			So(<-docChan, ShouldResemble, expectedReadOne)
			So(<-docChan, ShouldResemble, expectedReadTwo)
		})
	})
}

func TestCSVConvert(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("With a CSV input reader", t, func() {
		Convey("calling convert on a CSVConverter should return the expected BSON document", func() {
			csvConverter := CSVConverter{
				fields: []string{"field1", "field2", "field3"},
				data:   []string{"a", "b", "c"},
				index:  uint64(0),
			}
			expectedDocument := bson.D{
				bson.DocElem{"field1", "a"},
				bson.DocElem{"field2", "b"},
				bson.DocElem{"field3", "c"},
			}
			document, err := csvConverter.Convert()
			So(err, ShouldBeNil)
			So(document, ShouldResemble, expectedDocument)
		})
	})
}
