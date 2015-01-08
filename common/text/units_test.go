package text

import (
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestFormatByteCount(t *testing.T) {
	Convey("With some sample byte amounts", t, func() {
		Convey("0 Bytes -> 0 B", func() {
			So(FormatByteAmount(0), ShouldEqual, "0.0 B")
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
		Convey("5*1024*1024*1024*1024 Bytes -> 5120.0 GB", func() {
			So(FormatByteAmount(5*1024*1024*1024*1024), ShouldEqual, "5120.0 GB")
		})
	})
}

func TestOtherByteFormats(t *testing.T) {
	Convey("With some sample byte amounts", t, func() {
		Convey("with '10'", func() {
			Convey("FormatMegabyteAmount -> 10.0M", func() {
				So(FormatMegabyteAmount(10), ShouldEqual, "10.0M")
			})
			Convey("FormatByteAmount -> 10.0 B", func() {
				So(FormatByteAmount(10), ShouldEqual, "10.0 B")
			})
			Convey("FormatBitsWithLowPrecision -> 10b", func() {
				So(FormatBits(10), ShouldEqual, "10b")
			})
		})
		Convey("with '1024 * 2.5'", func() {
			val := int64(2.5 * 1024)
			Convey("FormatMegabyteAmount -> 2.5G", func() {
				So(FormatMegabyteAmount(val), ShouldEqual, "2.5G")
			})
			Convey("FormatByteAmount -> 2.5 KB", func() {
				So(FormatByteAmount(val), ShouldEqual, "2.5 KB")
			})
			Convey("FormatBitsWithLowPrecision -> 3k", func() {
				// 3 because it is bits instead of bytes, due to rounding
				So(FormatBits(val), ShouldEqual, "3k")
			})
		})
	})
}
