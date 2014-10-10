package text

import (
	"bytes"
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestWriteGrid(t *testing.T) {
	Convey("Test grid writer with no min width", t, func() {
		gw := GridWriter{}
		for i := 0; i < 3; i++ {
			for j := 0; j < 3; j++ {
				gw.WriteCell(fmt.Sprintf("(%v,%v)", i, j))
			}
			gw.EndRow()
		}
		buf := bytes.Buffer{}
		gw.Flush(&buf)
		output := buf.String()

		So(output, ShouldEqual,
			"(0,0)(0,1)(0,2)\n(1,0)(1,1)(1,2)\n(2,0)(2,1)(2,2)\n")

		gw.MinWidth = 7

		buf = bytes.Buffer{}
		gw.Flush(&buf)
		output = buf.String()
		So(output, ShouldStartWith,
			"  (0,0)  (0,1)  (0,2)\n  (1,0)  (1,1)")

		gw.MinWidth = 0
		gw.ColumnPadding = 1
		buf = bytes.Buffer{}
		gw.Flush(&buf)
		output = buf.String()
		So(output, ShouldStartWith,
			"(0,0) (0,1) (0,2)\n(1,0) (1,1)")
	})

	Convey("Test grid writer width calculation", t, func() {
		gw := GridWriter{}
		gw.WriteCell("bbbb")
		gw.WriteCell("aa")
		gw.WriteCell("c")
		gw.EndRow()
		gw.WriteCell("bb")
		gw.WriteCell("a")
		gw.WriteCell("")
		gw.EndRow()
		So(gw.calculateWidths(), ShouldResemble, []int{4, 2, 1})

		gw.WriteCell("bbbbbbb")
		gw.WriteCell("a")
		gw.WriteCell("cccc")
		gw.EndRow()
		So(gw.calculateWidths(), ShouldResemble, []int{7, 2, 4})

		gw.WriteCell("bbbbbbb")
		gw.WriteCell("a")
		gw.WriteCell("cccc")
		gw.WriteCell("ddddddddd")
		gw.EndRow()
		So(gw.calculateWidths(), ShouldResemble, []int{7, 2, 4, 9})
	})
}
