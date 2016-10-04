package options

import (
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestVerbosityFlag(t *testing.T) {
	Convey("With a new ToolOptions", t, func() {
		enabled := EnabledOptions{false, false, false}
		optPtr := New("", "", enabled)
		So(optPtr, ShouldNotBeNil)
		So(optPtr.parser, ShouldNotBeNil)

		Convey("no verbosity flags, Level should be 0", func() {
			_, err := optPtr.parser.ParseArgs([]string{})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 0)
		})

		Convey("one short verbosity flag, Level should be 1", func() {
			_, err := optPtr.parser.ParseArgs([]string{"-v"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 1)
		})

		Convey("three short verbosity flags (consecutive), Level should be 3", func() {
			_, err := optPtr.parser.ParseArgs([]string{"-vvv"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("three short verbosity flags (dispersed), Level should be 3", func() {
			_, err := optPtr.parser.ParseArgs([]string{"-v", "-v", "-v"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("short verbosity flag assigned to 3, Level should be 3", func() {
			_, err := optPtr.parser.ParseArgs([]string{"-v=3"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("consecutive short flags with assignment, only assignment holds", func() {
			_, err := optPtr.parser.ParseArgs([]string{"-vv=3"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("one long verbose flag, Level should be 1", func() {
			_, err := optPtr.parser.ParseArgs([]string{"--verbose"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 1)
		})

		Convey("three long verbosity flags, Level should be 3", func() {
			_, err := optPtr.parser.ParseArgs([]string{"--verbose", "--verbose", "--verbose"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("long verbosity flag assigned to 3, Level should be 3", func() {
			_, err := optPtr.parser.ParseArgs([]string{"--verbose=3"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 3)
		})

		Convey("mixed assignment and bare flag, total is sum", func() {
			_, err := optPtr.parser.ParseArgs([]string{"--verbose", "--verbose=3"})
			So(err, ShouldBeNil)
			So(optPtr.Level(), ShouldEqual, 4)
		})
	})
}
