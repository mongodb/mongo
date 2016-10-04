// +build failpoints

package failpoint

import (
	"testing"

	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
)

func TestFailpointParsing(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With test args", t, func() {
		args := "foo=bar,baz,biz=,=a"
		ParseFailpoints(args)

		So(Enabled("foo"), ShouldBeTrue)
		So(Enabled("baz"), ShouldBeTrue)
		So(Enabled("biz"), ShouldBeTrue)
		So(Enabled(""), ShouldBeTrue)
		So(Enabled("bar"), ShouldBeFalse)

		var val string
		var ok bool
		val, ok = Get("foo")
		So(val, ShouldEqual, "bar")
		So(ok, ShouldBeTrue)
		val, ok = Get("baz")
		So(val, ShouldEqual, "")
		So(ok, ShouldBeTrue)
		val, ok = Get("biz")
		So(val, ShouldEqual, "")
		So(ok, ShouldBeTrue)
		val, ok = Get("")
		So(val, ShouldEqual, "a")
		So(ok, ShouldBeTrue)
		val, ok = Get("bar")
		So(ok, ShouldBeFalse)
	})
}
