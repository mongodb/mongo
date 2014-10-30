package testutil

import (
	"flag"
	"strings"
	"testing"
)

const (
	// Integration tests require a mongod running on localhost:27017. If your
	// mongod uses SSL you need to specify the "ssl" type below, and ditto for
	// if your mongod requires auth.
	INTEGRATION_TEST_TYPE = "integration"

	// Unit tests don't require a real mongod. They may still do file I/O.
	UNIT_TEST_TYPE = "unit"

	// Kerberos tests are a special type of integration test that test tools
	// with Kerberos authentication against the drivers Kerberos testing cluster
	// because setting up a KDC every time is too brittle and expensive.
	// (See https://wiki.mongodb.com/display/DH/Testing+Kerberos)
	KERBEROS_TEST_TYPE = "kerberos"

	// "ssl" and "auth" are used to configure integration tests to run against
	// different mongod configurations. "ssl" will configure the integration tests
	// to expect an SSL-enabled mongod on localhost:27017. "auth" will do the same
	// for an auth-enabled mongod on localhost:27017.
	SSL_TEST_TYPE  = "ssl"
	AUTH_TEST_TYPE = "auth"
)

var (
	// the types of tests that should be run
	testTypes = flag.String("test.types", UNIT_TEST_TYPE, "Comma-separated list of the"+
		" types of tests to be run")
	// above, split on the comma
	testTypesParsed []string
)

func HasTestType(testType string) bool {
	// parse if necessary
	if testTypesParsed == nil {
		testTypesParsed = strings.Split(*testTypes, ",")
	}

	// skip the test if the passed-in type is not being run
	for _, typ := range testTypesParsed {
		if typ == testType {
			return true
		}
	}
	return false
}

// Skip the test if the specified type is not being run.
func VerifyTestType(t *testing.T, testType string) {
	if !HasTestType(testType) {
		t.SkipNow()
	}
}
