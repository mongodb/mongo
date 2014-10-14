package system

import (
	"errors"
	"fmt"
	"strings"
	"testing"

	. "github.com/smartystreets/goconvey/convey"
)

func TestShell(t *testing.T) {
	for i, test := range cases {
		Convey(fmt.Sprintf("%d - %s", i, test.String()), t, func() {
			fmt.Printf("\n%s\n\n", test.String())
			output, err := invokeShell(test)

			So(output, ShouldEqual, test.output)
			So(err, ShouldResemble, test.err)
		})
	}
}

func invokeShell(test TestCase) (string, error) {
	executor := NewCommandRecorder(test)
	shell := NewShell(executor, "go", test.short, test.coverage, "reports")
	return shell.GoTest("directory", "pack/age")
}

var cases = []TestCase{
	TestCase{
		imports: false,
		output:  "import compilation",
		err:     errors.New("directory|go test -i"),
	},
	TestCase{
		imports: true, short: false, coverage: false, goconvey: false, passes: false,
		output: "test execution",
		err:    errors.New("directory|go test -v -short=false"),
	},
	TestCase{
		imports: true, short: false, coverage: false, goconvey: false, passes: true,
		output: "test execution",
		err:    nil,
	},
	TestCase{
		imports: true, short: false, coverage: false, goconvey: true, passes: false,
		output: "goconvey test execution",
		err:    errors.New("directory|go test -v -short=false -json"),
	},
	TestCase{
		imports: true, short: false, coverage: false, goconvey: true, passes: true,
		output: "goconvey test execution",
		err:    nil,
	},
	TestCase{
		imports: true, short: false, coverage: true, goconvey: false, passes: false,
		output: "test execution", // because the tests fail with coverage, they are re-run without coverage
		err:    errors.New("directory|go test -v -short=false"),
	},
	TestCase{
		imports: true, short: false, coverage: true, goconvey: false, passes: true,
		output: "test coverage execution",
		err:    nil,
	},
	// TestCase{
	// 	imports: true, short: false, coverage: true, goconvey: true, passes: false,
	// 	output: "test execution", // because the tests fail with coverage, they are re-run without coverage
	// 	err:    errors.New("directory|go test -v -short=false -json"),
	// },
	// TestCase{
	// 	imports: true, short: false, coverage: true, goconvey: true, passes: true,
	// },
	// TestCase{
	// 	imports: true, short: true, coverage: false, goconvey: false, passes: false,
	// },
	// TestCase{
	// 	imports: true, short: true, coverage: false, goconvey: false, passes: true,
	// },
	// TestCase{
	// 	imports: true, short: true, coverage: false, goconvey: true, passes: false,
	// },
	// TestCase{
	// 	imports: true, short: true, coverage: false, goconvey: true, passes: true,
	// },
	// TestCase{
	// 	imports: true, short: true, coverage: true, goconvey: false, passes: false,
	// },
	// TestCase{
	// 	imports: true, short: true, coverage: true, goconvey: false, passes: true,
	// },
	// TestCase{
	// 	imports: true, short: true, coverage: true, goconvey: true, passes: false,
	// },
	// TestCase{
	// 	imports: true, short: true, coverage: true, goconvey: true, passes: true,
	// },
}

type TestCase struct {

	// input parameters
	imports  bool // is `go test -i`  successful?
	short    bool // is `-short` enabled?
	coverage bool // is `-coverage` enabled?
	goconvey bool // do the tests use the GoConvey DSL?
	passes   bool // do the tests pass?

	// expected results
	output string
	err    error
}

func (self TestCase) String() string {
	return fmt.Sprintf("Parameters: | %s | %s | %s | %s | %s |",
		decideCase("imports", self.imports),
		decideCase("short", self.short),
		decideCase("coverage", self.coverage),
		decideCase("goconvey", self.goconvey),
		decideCase("passes", self.passes))
}

// state == true: UPPERCASE
// state == false: lowercase
func decideCase(text string, state bool) string {
	if state {
		return strings.ToUpper(text)
	}
	return strings.ToLower(text)
}
