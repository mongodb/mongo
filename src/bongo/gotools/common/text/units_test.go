package text

import (
	"testing"

	. "github.com/smartystreets/goconvey/convey"
)

func TestFormatByteCount(t *testing.T) {
	Convey("With some sample byte amounts", t, func() {
		Convey("0 Bytes -> 0B", func() {
			So(FormatByteAmount(0), ShouldEqual, "0B")
		})
		Convey("1024 Bytes -> 1.00KB", func() {
			So(FormatByteAmount(1024), ShouldEqual, "1.00KB")
		})
		Convey("2500 Bytes -> 2.44KB", func() {
			So(FormatByteAmount(2500), ShouldEqual, "2.44KB")
		})
		Convey("2*1024*1024 Bytes -> 2.00MB", func() {
			So(FormatByteAmount(2*1024*1024), ShouldEqual, "2.00MB")
		})
		Convey("5*1024*1024*1024 Bytes -> 5.00GB", func() {
			So(FormatByteAmount(5*1024*1024*1024), ShouldEqual, "5.00GB")
		})
		Convey("5*1024*1024*1024*1024 Bytes -> 5120GB", func() {
			So(FormatByteAmount(5*1024*1024*1024*1024), ShouldEqual, "5120GB")
		})
	})
}

func TestOtherByteFormats(t *testing.T) {
	Convey("With some sample byte amounts", t, func() {
		Convey("with '10'", func() {
			Convey("FormatMegabyteAmount -> 10.0M", func() {
				So(FormatMegabyteAmount(10), ShouldEqual, "10.0M")
			})
			Convey("FormatByteAmount -> 10B", func() {
				So(FormatByteAmount(10), ShouldEqual, "10B")
			})
			Convey("FormatBitsWithLowPrecision -> 10b", func() {
				So(FormatBits(10), ShouldEqual, "10b")
			})
		})
		Convey("with '1024 * 2.5'", func() {
			val := int64(2.5 * 1024)
			Convey("FormatMegabyteAmount -> 2.50G", func() {
				So(FormatMegabyteAmount(val), ShouldEqual, "2.50G")
			})
			Convey("FormatByteAmount -> 2.50KB", func() {
				So(FormatByteAmount(val), ShouldEqual, "2.50KB")
			})
			Convey("FormatBits -> 2.56k", func() {
				So(FormatBits(val), ShouldEqual, "2.56k")
			})
		})
	})
}

func TestBitFormatPrecision(t *testing.T) {
	Convey("With values less than 1k", t, func() {
		Convey("with '999'", func() {
			Convey("FormatBits -> 999b", func() {
				So(FormatBits(999), ShouldEqual, "999b")
			})
		})
		Convey("with '99'", func() {
			Convey("FormatBits -> 99b", func() {
				So(FormatBits(99), ShouldEqual, "99b")
			})
		})
		Convey("with '9'", func() {
			Convey("FormatBits -> 9b", func() {
				So(FormatBits(9), ShouldEqual, "9b")
			})
		})
	})
	Convey("With values less than 1m", t, func() {
		Convey("with '9999'", func() {
			Convey("FormatBits -> 10.0k", func() {
				So(FormatBits(9999), ShouldEqual, "10.0k")
			})
		})
		Convey("with '9990'", func() {
			Convey("FormatBits -> 9.99k", func() {
				So(FormatBits(9990), ShouldEqual, "9.99k")
			})
		})
	})
	Convey("With big numbers", t, func() {
		Convey("with '999000000'", func() {
			Convey("FormatBits -> 999m", func() {
				So(FormatBits(999000000), ShouldEqual, "999m")
			})
		})
		Convey("with '9990000000'", func() {
			Convey("FormatBits -> 9.99g", func() {
				So(FormatBits(9990000000), ShouldEqual, "9.99g")
			})
		})
	})
}
