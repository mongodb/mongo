// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package testutil

import (
	"flag"
	"strings"
	"testing"
)

const (
	// Integration tests require a mongod running on localhost:33333. If your
	// mongod uses SSL you need to specify the "ssl" type below, and ditto for
	// if your mongod requires auth.
	IntegrationTestType = "integration"

	// Unit tests don't require a real mongod. They may still do file I/O.
	UnitTestType = "unit"

	// Kerberos tests are a special type of integration test that test tools
	// with Kerberos authentication against the drivers Kerberos testing cluster
	// because setting up a KDC every time is too brittle and expensive.
	// (See https://wiki.mongodb.com/display/DH/Testing+Kerberos)
	KerberosTestType = "kerberos"

	// "ssl" and "auth" are used to configure integration tests to run against
	// different mongod configurations. "ssl" will configure the integration tests
	// to expect an SSL-enabled mongod on localhost:33333. "auth" will do the same
	// for an auth-enabled mongod on localhost:33333.
	SSLTestType  = "ssl"
	AuthTestType = "auth"
)

var (
	// the types of tests that should be run
	testTypes = flag.String("test.types", UnitTestType, "Comma-separated list of the"+
		" types of tests to be run")
)

func HasTestType(testType string) bool {
	if !flag.Parsed() {
		flag.Parse()
	}

	// skip the test if the passed-in type is not being run
	for _, typ := range strings.Split(*testTypes, ",") {
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
