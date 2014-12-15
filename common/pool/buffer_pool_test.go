package pool

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestBufferPoolAllocation(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	var bp1, bp2, bp3 *BufferPool

	Convey("A new BufferPool of negative size should panic", t, func() {
		So(func() { NewBufferPool(-5) }, ShouldPanicWith,
			"cannot create BufferPool of negative size")
	})

	Convey("With some BufferPools of different sizes", t, func() {
		bp1 = NewBufferPool(4)
		bp2 = NewBufferPool(1024)
		bp3 = NewBufferPool(1024 * 1024)
		So(bp1, ShouldNotBeNil)
		So(bp1.pool, ShouldNotBeNil)
		So(bp1.pool.New, ShouldNotBeNil)
		So(bp2, ShouldNotBeNil)
		So(bp3, ShouldNotBeNil)
		So(bp1.size, ShouldEqual, 4)
		So(bp2.size, ShouldEqual, 1024)
		So(bp3.size, ShouldEqual, 1024*1024)

		Convey("each buffer should return the proper size", func() {
			buff1 := bp1.Get()
			So(len(buff1), ShouldEqual, 4)
			buff2 := bp2.Get()
			So(len(buff2), ShouldEqual, 1024)
			buff3 := bp3.Get()
			So(len(buff3), ShouldEqual, 1024*1024)

			Convey("and returning the buffers to their pools should be fine", func() {
				So(func() { bp1.Put(buff1) }, ShouldNotPanic)
				So(func() { bp2.Put(buff2) }, ShouldNotPanic)
				So(func() { bp3.Put(buff3) }, ShouldNotPanic)
			})

			Convey("but returning a buffer of the wrong size should panic", func() {
				So(func() { bp1.Put(buff2) }, ShouldPanic)
				So(func() { bp2.Put(buff3) }, ShouldPanic)
				So(func() { bp3.Put(buff1) }, ShouldPanic)
				So(func() { bp1.Put(buff1[1:]) }, ShouldPanic)
				So(func() { bp2.Put(buff2[50:500]) }, ShouldPanic)
				So(func() { bp3.Put(buff3[:1]) }, ShouldPanic)
			})
		})
	})
}
