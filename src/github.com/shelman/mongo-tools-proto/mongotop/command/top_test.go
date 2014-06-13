package command

import (
	"github.com/shelman/mongo-tools-proto/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestTopCommandDiff(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("When diffing two Top commands", t, func() {

		var firstTop *Top
		var secondTop *Top

		Convey("any namespaces only existing in the first Top should be"+
			" ignored", func() {

			firstTop = &Top{
				Totals: map[string]NSTopInfo{
					"a": NSTopInfo{},
					"b": NSTopInfo{},
					"c": NSTopInfo{},
				},
			}

			secondTop = &Top{
				Totals: map[string]NSTopInfo{
					"a": NSTopInfo{},
				},
			}

			diff, err := secondTop.Diff(firstTop)
			So(err, ShouldBeNil)

			asTopDiff, ok := diff.(*TopDiff)
			So(ok, ShouldBeTrue)

			_, hasA := asTopDiff.Totals["a"]
			So(hasA, ShouldBeTrue)
			_, hasB := asTopDiff.Totals["b"]
			So(hasB, ShouldBeFalse)
			_, hasC := asTopDiff.Totals["c"]
			So(hasC, ShouldBeFalse)

		})

		Convey("any namespaces only existing in the second Top should be"+
			" ignored", func() {

			firstTop = &Top{
				Totals: map[string]NSTopInfo{
					"a": NSTopInfo{},
				},
			}

			secondTop = &Top{
				Totals: map[string]NSTopInfo{
					"a": NSTopInfo{},
					"b": NSTopInfo{},
					"c": NSTopInfo{},
				},
			}

			diff, err := secondTop.Diff(firstTop)
			So(err, ShouldBeNil)

			asTopDiff, ok := diff.(*TopDiff)
			So(ok, ShouldBeTrue)

			_, hasA := asTopDiff.Totals["a"]
			So(hasA, ShouldBeTrue)
			_, hasB := asTopDiff.Totals["b"]
			So(hasB, ShouldBeFalse)
			_, hasC := asTopDiff.Totals["c"]
			So(hasC, ShouldBeFalse)

		})

		Convey("the differences for the times for any shared namespaces should"+
			"be included in the output", func() {

			firstTop = &Top{
				Totals: map[string]NSTopInfo{
					"a": NSTopInfo{
						Total: TopField{
							Time: 2,
						},
						Read: TopField{
							Time: 2,
						},
						Write: TopField{
							Time: 2,
						},
					},
					"b": NSTopInfo{
						Total: TopField{
							Time: 2,
						},
						Read: TopField{
							Time: 2,
						},
						Write: TopField{
							Time: 2,
						},
					},
				},
			}

			secondTop = &Top{
				Totals: map[string]NSTopInfo{
					"a": NSTopInfo{
						Total: TopField{
							Time: 3,
						},
						Read: TopField{
							Time: 3,
						},
						Write: TopField{
							Time: 3,
						},
					},
					"b": NSTopInfo{
						Total: TopField{
							Time: 4,
						},
						Read: TopField{
							Time: 4,
						},
						Write: TopField{
							Time: 4,
						},
					},
				},
			}

			diff, err := secondTop.Diff(firstTop)
			So(err, ShouldBeNil)

			asTopDiff, ok := diff.(*TopDiff)
			So(ok, ShouldBeTrue)

			So(asTopDiff.Totals["a"], ShouldResemble, []int{1, 1, 1})
			So(asTopDiff.Totals["b"], ShouldResemble, []int{2, 2, 2})

		})

	})

}

func TestTopDiffToRows(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("When converting a TopDiff to rows to be printed", t, func() {

		var diff *TopDiff

		Convey("the first row should contain the appropriate column"+
			" headers", func() {

			diff = &TopDiff{
				Totals: map[string][]int{},
			}

			rows := diff.ToRows()
			So(len(rows), ShouldEqual, 1)
			headerRow := rows[0]
			So(len(headerRow), ShouldEqual, 5)
			So(headerRow[0], ShouldEqual, "ns")
			So(headerRow[1], ShouldEqual, "total")
			So(headerRow[2], ShouldEqual, "read")
			So(headerRow[3], ShouldEqual, "write")

		})

		Convey("the subsequent rows should contain the correct totals from"+
			" the diff", func() {

			diff = &TopDiff{
				Totals: map[string][]int{
					"a.b": []int{0, 1000, 2000},
					"c.d": []int{2000, 1000, 0},
				},
			}

			rows := diff.ToRows()
			So(len(rows), ShouldEqual, 3)
			So(rows[1][0], ShouldEqual, "a.b")
			So(rows[1][1], ShouldEqual, "0ms")
			So(rows[1][2], ShouldEqual, "1ms")
			So(rows[1][3], ShouldEqual, "2ms")
			So(rows[2][0], ShouldEqual, "c.d")
			So(rows[2][1], ShouldEqual, "2ms")
			So(rows[2][2], ShouldEqual, "1ms")
			So(rows[2][3], ShouldEqual, "0ms")

		})

		Convey("the namespaces should appear in alphabetical order", func() {

			diff = &TopDiff{
				Totals: map[string][]int{
					"a.b": []int{},
					"a.c": []int{},
					"a.a": []int{},
				},
			}

			rows := diff.ToRows()
			So(len(rows), ShouldEqual, 4)
			So(rows[1][0], ShouldEqual, "a.a")
			So(rows[2][0], ShouldEqual, "a.b")
			So(rows[3][0], ShouldEqual, "a.c")

		})

		Convey("any negative values should be capped to 0", func() {

			diff = &TopDiff{
				Totals: map[string][]int{
					"a.b": []int{-1000, 5000, -3000},
				},
			}

			rows := diff.ToRows()
			So(len(rows), ShouldEqual, 2)
			So(rows[1][1], ShouldEqual, "0ms")
			So(rows[1][2], ShouldEqual, "5ms")
			So(rows[1][3], ShouldEqual, "0ms")

		})

		Convey("any namespaces that are just a database, are from the local"+
			"database, or are a collection of namespaces should be"+
			" skipped", func() {

			diff := &TopDiff{
				Totals: map[string][]int{
					"a.b":          []int{},
					"local.b":      []int{},
					"a.namespaces": []int{},
					"a":            []int{},
				},
			}

			rows := diff.ToRows()
			So(len(rows), ShouldEqual, 2)
			So(rows[1][0], ShouldEqual, "a.b")

		})

	})

}
