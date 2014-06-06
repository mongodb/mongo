package testutil

import (
	"flag"
	"strings"
	"testing"
)

var (
	// the types of tests that should be run
	testTypes = flag.String("test.types", "unit", "Comma-separated list of the"+
		" types of tests to be run")
	// above, split on the comma
	testTypesParsed []string
)

// Skip the test if the specified type is not being run.
func VerifyTestType(t *testing.T, testType string) {

	// parse if necessary
	if testTypesParsed == nil {
		testTypesParsed = strings.Split(*testTypes, ",")
	}

	// skip the test if the passed-in type is not being run
	for _, typ := range testTypesParsed {
		if typ == testType {
			return
		}
	}
	t.SkipNow()

}
