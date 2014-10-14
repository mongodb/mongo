package parser

import (
	"testing"

	. "github.com/smartystreets/goconvey/convey"
	"github.com/smartystreets/goconvey/web/server/contract"
)

func TestParser(t *testing.T) {
	var (
		packages = []*contract.Package{
			&contract.Package{Active: true, Output: "Active!", Result: contract.NewPackageResult("asdf")},
			&contract.Package{Active: false, Output: "Inactive!", Result: contract.NewPackageResult("qwer")},
		}
	)

	Convey("Subject: Parser parses test output for active packages", t, func() {
		parser := NewParser(fakeParserImplementation)

		Convey("When given a collection of packages", func() {
			parser.Parse(packages)

			Convey("The parser uses its internal parsing mechanism to parse the output of only the active packages", func() {
				So(packages[0].Result.Outcome, ShouldEqual, packages[0].Output)
			})

			Convey("The parser should mark inactive packages as ignored", func() {
				So(packages[1].Result.Outcome, ShouldEqual, contract.Ignored)
			})
		})
	})
}

func fakeParserImplementation(result *contract.PackageResult, rawOutput string) {
	result.Outcome = rawOutput
}
