package result

import (
	"github.com/shelman/mongo-tools-proto/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestDiff(t *testing.T) {

	testutil.VerifyTestType(t, "unit")

	Convey("When diffing two top results", t, func() {

		Convey("any namespaces appearing in both results should show up in the"+
			" diff", func() {

			first := &TopResults{
				Totals: map[string]NSTopInfo{
					"nsOne": NSTopInfo{
						Total: TopField{
							Time:  0,
							Count: 0,
						},
						Read: TopField{
							Time:  0,
							Count: 0,
						},
						Write: TopField{
							Time:  0,
							Count: 0,
						},
					},
					"nsTwo": NSTopInfo{
						Total: TopField{
							Time:  0,
							Count: 0,
						},
						Read: TopField{
							Time:  0,
							Count: 0,
						},
						Write: TopField{
							Time:  0,
							Count: 0,
						},
					},
				},
			}

			second := &TopResults{
				Totals: map[string]NSTopInfo{
					"nsOne": NSTopInfo{
						Total: TopField{
							Time:  1,
							Count: 1,
						},
						Read: TopField{
							Time:  1,
							Count: 1,
						},
						Write: TopField{
							Time:  1,
							Count: 1,
						},
					},
					"nsTwo": NSTopInfo{
						Total: TopField{
							Time:  2,
							Count: 2,
						},
						Read: TopField{
							Time:  2,
							Count: 2,
						},
						Write: TopField{
							Time:  2,
							Count: 2,
						},
					},
				},
			}

			diff := Diff(first, second)

			So(
				diff.Totals["nsOne"],
				ShouldResemble,
				NSTopInfo{
					Total: TopField{
						Time:  1,
						Count: 1,
					},
					Read: TopField{
						Time:  1,
						Count: 1,
					},
					Write: TopField{
						Time:  1,
						Count: 1,
					},
				},
			)

			So(
				diff.Totals["nsTwo"],
				ShouldResemble,
				NSTopInfo{
					Total: TopField{
						Time:  2,
						Count: 2,
					},
					Read: TopField{
						Time:  2,
						Count: 2,
					},
					Write: TopField{
						Time:  2,
						Count: 2,
					},
				},
			)

		})

		Convey("any namespaces only showing up in only one of the two results"+
			" should be excluded from the diff", func() {

			first := &TopResults{
				Totals: map[string]NSTopInfo{
					"nsOne": NSTopInfo{
						Total: TopField{
							Time:  0,
							Count: 0,
						},
						Read: TopField{
							Time:  0,
							Count: 0,
						},
						Write: TopField{
							Time:  0,
							Count: 0,
						},
					},
					"nsTwo": NSTopInfo{
						Total: TopField{
							Time:  0,
							Count: 0,
						},
						Read: TopField{
							Time:  0,
							Count: 0,
						},
						Write: TopField{
							Time:  0,
							Count: 0,
						},
					},
				},
			}

			second := &TopResults{
				Totals: map[string]NSTopInfo{
					"nsOne": NSTopInfo{
						Total: TopField{
							Time:  1,
							Count: 1,
						},
						Read: TopField{
							Time:  1,
							Count: 1,
						},
						Write: TopField{
							Time:  1,
							Count: 1,
						},
					},
					"nsThree": NSTopInfo{
						Total: TopField{
							Time:  3,
							Count: 3,
						},
						Read: TopField{
							Time:  3,
							Count: 3,
						},
						Write: TopField{
							Time:  3,
							Count: 3,
						},
					},
				},
			}

			diff := Diff(first, second)

			So(
				diff.Totals["nsOne"],
				ShouldResemble,
				NSTopInfo{
					Total: TopField{
						Time:  1,
						Count: 1,
					},
					Read: TopField{
						Time:  1,
						Count: 1,
					},
					Write: TopField{
						Time:  1,
						Count: 1,
					},
				},
			)

			_, ok := diff.Totals["nsTwo"]
			So(ok, ShouldBeFalse)
			_, ok = diff.Totals["nsThree"]
			So(ok, ShouldBeFalse)

		})

		Convey("diffs should have a minumum value of 0", func() {

			first := &TopResults{
				Totals: map[string]NSTopInfo{
					"nsOne": NSTopInfo{
						Total: TopField{
							Time:  1,
							Count: 1,
						},
						Read: TopField{
							Time:  1,
							Count: 1,
						},
						Write: TopField{
							Time:  1,
							Count: 1,
						},
					},
				},
			}

			second := &TopResults{
				Totals: map[string]NSTopInfo{
					"nsOne": NSTopInfo{
						Total: TopField{
							Time:  0,
							Count: 0,
						},
						Read: TopField{
							Time:  0,
							Count: 0,
						},
						Write: TopField{
							Time:  0,
							Count: 0,
						},
					},
				},
			}

			diff := Diff(first, second)

			So(
				diff.Totals["nsOne"],
				ShouldResemble,
				NSTopInfo{
					Total: TopField{
						Time:  0,
						Count: 0,
					},
					Read: TopField{
						Time:  0,
						Count: 0,
					},
					Write: TopField{
						Time:  0,
						Count: 0,
					},
				},
			)

		})

	})

}
