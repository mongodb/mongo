package mongoexport

import (
	"bytes"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"testing"
)

func TestWriteCSV(t *testing.T) {
	testutil.VerifyTestType(t, "unit")

	Convey("With a CSV export output", t, func() {
		fields := []string{"_id", "x", " y", "z.1.a"}
		out := &bytes.Buffer{}

		Convey("Headers should be written correctly", func() {
			csvExporter := NewCSVExportOutput(fields, out)
			err := csvExporter.WriteHeader()
			So(err, ShouldBeNil)
			csvExporter.WriteFooter()
		})

		Convey("Exported document with missing fields should print as blank", func() {
			csvExporter := NewCSVExportOutput(fields, out)
			csvExporter.ExportDocument(bson.M{"_id": "12345"})
			csvExporter.WriteFooter()
			csvExporter.Flush()
			So(out.String(), ShouldEqual, `12345,"","",""`+"\n")
		})

		Convey("Exported document with index into nested objects should print correctly", func() {
			csvExporter := NewCSVExportOutput(fields, out)
			csvExporter.ExportDocument(bson.M{"z": []interface{}{"x", bson.M{"a": "T", "B": 1}}})
			csvExporter.WriteFooter()
			csvExporter.Flush()
			So(out.String(), ShouldEqual, `"","","",T`+"\n")
		})

		Reset(func() {
			out.Reset()
		})

	})
}
