package mongoexport

import (
	"bytes"
	//"fmt"
	"encoding/json"
	. "github.com/smartystreets/goconvey/convey"
	"labix.org/v2/mgo/bson"
	"testing"
)

func TestWriteJSON(t *testing.T) {
	Convey("With a JSON export output", t, func() {
		out := &bytes.Buffer{}

		Convey("Special types should serialize as extended JSON", func() {

			Convey("ObjectId should have an extended JSON format", func() {
				jsonExporter := NewJSONExportOutput(false, out)
				objId := bson.NewObjectId()
				err := jsonExporter.WriteHeader()
				So(err, ShouldBeNil)
				err = jsonExporter.ExportDocument(bson.M{"_id": objId})
				So(err, ShouldBeNil)
				err = jsonExporter.WriteFooter()
				So(err, ShouldBeNil)
				So(out.String(), ShouldEqual, `{"_id":{"$oid":"`+objId.Hex()+`"}}`+"\n")
			})

			Reset(func() {
				out.Reset()
			})
		})

	})
}

func TestJSONArray(t *testing.T) {
	Convey("With a JSON export output in array mode", t, func() {
		out := &bytes.Buffer{}
		Convey("exporting a bunch of documents should produce valid json", func() {
			jsonExporter := NewJSONExportOutput(true, out)
			err := jsonExporter.WriteHeader()
			So(err, ShouldBeNil)

			// Export a few docs of various types

			testObjs := []interface{}{bson.NewObjectId(), "asd", 12345, 3.14159, bson.M{"A": 1}}
			for _, obj := range testObjs {
				err = jsonExporter.ExportDocument(bson.M{"_id": obj})
				So(err, ShouldBeNil)
			}

			err = jsonExporter.WriteFooter()
			So(err, ShouldBeNil)

			//Unmarshal the whole thing, it should be valid json
			fromJSON := []map[string]interface{}{}
			err = json.Unmarshal(out.Bytes(), &fromJSON)
			So(err, ShouldBeNil)
			So(len(fromJSON), ShouldEqual, len(testObjs))

		})

		Reset(func() {
			out.Reset()
		})

	})
}
