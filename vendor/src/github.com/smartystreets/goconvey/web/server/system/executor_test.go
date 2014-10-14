package system

import (
	"errors"
	"fmt"
	"strings"
)

type CommandRecorder struct {
	test TestCase
}

func (self *CommandRecorder) Execute(directory, name string, arguments ...string) (output string, err error) {
	concatenated := fmt.Sprintf("%s|%s %s", directory, name, strings.Join(arguments, " "))
	concatenated = strings.TrimSpace(concatenated)
	output = self.Output(concatenated)
	err = self.Error(concatenated)
	fmt.Println("$ ", concatenated, output, "<error>", err, "</error>", "\n")
	return
}

func (self *CommandRecorder) Output(invocation string) string {
	output := outputs[invocation]
	if output == goconveyDSLImport && !self.test.goconvey {
		output = goConveyNotFound
	}
	return output
}

func (self *CommandRecorder) Error(invocation string) error {
	if invocation == compileImports && self.test.imports {
		return nil
	} else if invocation == compileImports {
		return errors.New(compileImports)
	}

	if invocation == detectGoConvey {
		return nil
	}

	if invocation == executeTests && self.test.passes {
		return nil
	} else if invocation == executeTests {
		return errors.New(executeTests)
	}

	if invocation == executeGoConvey && self.test.passes {
		return nil
	} else if invocation == executeGoConvey {
		return errors.New(executeGoConvey)
	}

	if invocation == coverTests && self.test.passes {
		return nil
	} else if invocation == coverTests {
		return errors.New(coverTests)
	}

	if invocation == coverGoConvey && self.test.passes {
		return nil
	} else if invocation == coverGoConvey {
		return errors.New(coverTests)
	}

	return nil
}

func NewCommandRecorder(test TestCase) *CommandRecorder {
	self := new(CommandRecorder)
	self.test = test
	return self
}

const (
	compileImports   = "directory|go test -i"
	detectGoConvey   = "directory|go list -f '{{.TestImports}}' pack/age"
	goConveyNotFound = "Don't let this look like a GoConvey test suite!"
	executeTests     = "directory|go test -v -short=false"
	executeGoConvey  = "directory|go test -v -short=false -json"
	coverTests       = "directory|go test -v -short=false -covermode=set -coverprofile=reports/pack-age.txt"
	coverGoConvey    = "directory|go test -v -short=false -covermode=set -coverprofile=reports/pack-age.txt -json"
	profileTests     = "directory|go tool cover -html=reports/pack-age.txt -o reports/pack-age.html"
)

var outputs = map[string]string{
	compileImports:  "import compilation",
	detectGoConvey:  goconveyDSLImport,
	executeTests:    "test execution",
	executeGoConvey: "goconvey test execution",
	coverTests:      "test coverage execution",
	coverGoConvey:   "goconvey coverage execution",
}
