package mongoexport

import (
	"bytes"
	"encoding/csv"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"strings"
	"testing"
)

func TestWriteCSV(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

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
			rec, err := csv.NewReader(strings.NewReader(out.String())).Read()
			So(err, ShouldBeNil)
			So(rec, ShouldResemble, []string{"12345", "", "", ""})
		})

		Convey("Exported document with index into nested objects should print correctly", func() {
			csvExporter := NewCSVExportOutput(fields, out)
			csvExporter.ExportDocument(bson.M{"z": []interface{}{"x", bson.M{"a": "T", "B": 1}}})
			csvExporter.WriteFooter()
			csvExporter.Flush()
			rec, err := csv.NewReader(strings.NewReader(out.String())).Read()
			So(err, ShouldBeNil)
			So(rec, ShouldResemble, []string{"", "", "", "T"})
		})

		Reset(func() {
			out.Reset()
		})

	})
}
