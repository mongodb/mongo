// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package text

import (
	"bytes"
	"fmt"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestUpdateWidths(t *testing.T) {
	Convey("Using a grid writer, the cached column width", t, func() {
		gw := GridWriter{}
		defaultWidths := []int{1, 2, 3, 4}
		Convey("should be updated when one or more new cell widths are greater", func() {
			// the first time, the grid's widths are nil
			So(gw.colWidths, ShouldBeNil)
			gw.updateWidths(defaultWidths)
			So(gw.colWidths, ShouldResemble, defaultWidths)

			// the grid's widths should not be updated if all the new cell widths are less than or equal
			newWidths := []int{1, 2, 1, 2}
			So(gw.colWidths, ShouldNotBeNil)
			gw.updateWidths(newWidths)
			So(gw.colWidths, ShouldResemble, defaultWidths)
			So(gw.colWidths, ShouldNotResemble, newWidths)

			// the grid's widths should be updated if any of the new cell widths are greater
			newWidths = []int{1, 2, 3, 5}
			So(gw.colWidths, ShouldNotBeNil)
			gw.updateWidths(newWidths)
			So(gw.colWidths, ShouldResemble, newWidths)
			So(gw.colWidths, ShouldNotResemble, defaultWidths)
		})
	})
}

func writeData(gw *GridWriter) {
	gw.Reset()
	for i := 0; i < 3; i++ {
		for j := 0; j < 3; j++ {
			gw.WriteCell(fmt.Sprintf("(%v,%v)", i, j))
		}
		gw.EndRow()
	}
}

func TestWriteGrid(t *testing.T) {
	Convey("Test grid writer with no min width", t, func() {
		gw := GridWriter{}
		writeData(&gw)
		buf := bytes.Buffer{}
		gw.Flush(&buf)
		So(buf.String(), ShouldEqual,
			"(0,0)(0,1)(0,2)\n(1,0)(1,1)(1,2)\n(2,0)(2,1)(2,2)\n")

		writeData(&gw)
		gw.MinWidth = 7
		buf = bytes.Buffer{}
		gw.Flush(&buf)
		So(buf.String(), ShouldStartWith,
			"  (0,0)  (0,1)  (0,2)\n  (1,0)  (1,1)")

		writeData(&gw)
		gw.colWidths = []int{}
		gw.MinWidth = 0
		gw.ColumnPadding = 1
		buf = bytes.Buffer{}
		gw.Flush(&buf)
		So(buf.String(), ShouldStartWith,
			"(0,0) (0,1) (0,2)\n(1,0) (1,1)")

		writeData(&gw)
		buf = bytes.Buffer{}
		gw.FlushRows(&buf)
		So(buf.String(), ShouldStartWith,
			"(0,0) (0,1) (0,2)(1,0) (1,1)")
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
