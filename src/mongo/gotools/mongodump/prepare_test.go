package mongodump

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestSkipCollection(t *testing.T) {

	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With a mongodump that excludes collections 'test' and 'fake'"+
		" and excludes prefixes 'pre-' and 'no'", t, func() {
		md := &MongoDump{
			OutputOptions: &OutputOptions{
				ExcludedCollections:        []string{"test", "fake"},
				ExcludedCollectionPrefixes: []string{"pre-", "no"},
			},
		}

		Convey("collection 'pre-test' should be skipped", func() {
			So(md.shouldSkipCollection("pre-test"), ShouldBeTrue)
		})

		Convey("collection 'notest' should be skipped", func() {
			So(md.shouldSkipCollection("notest"), ShouldBeTrue)
		})

		Convey("collection 'test' should be skipped", func() {
			So(md.shouldSkipCollection("test"), ShouldBeTrue)
		})

		Convey("collection 'fake' should be skipped", func() {
			So(md.shouldSkipCollection("fake"), ShouldBeTrue)
		})

		Convey("collection 'fake222' should not be skipped", func() {
			So(md.shouldSkipCollection("fake222"), ShouldBeFalse)
		})

		Convey("collection 'random' should not be skipped", func() {
			So(md.shouldSkipCollection("random"), ShouldBeFalse)
		})

		Convey("collection 'mytest' should not be skipped", func() {
			So(md.shouldSkipCollection("mytest"), ShouldBeFalse)
		})
	})

}
