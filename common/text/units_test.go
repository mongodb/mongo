package text

import (
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestFormatByteCount(t *testing.T) {
	Convey("With some sample byte amounts", t, func() {
		Convey("0 Bytes -> 0 KB", func() {
			So(FormatByteAmount(0), ShouldEqual, "0.0 KB")
		})
		Convey("1024 Bytes -> 1 KB", func() {
			So(FormatByteAmount(1024), ShouldEqual, "1.0 KB")
		})
		Convey("2500 Bytes -> 2.4 KB", func() {
			So(FormatByteAmount(2500), ShouldEqual, "2.4 KB")
		})
		Convey("2*1024*1024 Bytes -> 2.0 MB", func() {
			So(FormatByteAmount(2*1024*1024), ShouldEqual, "2.0 MB")
		})
		Convey("5*1024*1024*1024 Bytes -> 5.0 GB", func() {
			So(FormatByteAmount(5*1024*1024*1024), ShouldEqual, "5.0 GB")
		})
	})
}
