package util

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestMaxInt(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("When finding the maximum of two ints", t, func() {

		Convey("the larger int should be returned", func() {

			So(MaxInt(1, 2), ShouldEqual, 2)
			So(MaxInt(2, 1), ShouldEqual, 2)

		})

	})
}
