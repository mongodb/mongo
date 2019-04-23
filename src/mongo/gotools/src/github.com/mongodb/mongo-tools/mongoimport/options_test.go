// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoimport

import (
	"testing"

	"go.mongodb.org/mongo-driver/mongo/writeconcern"

	"github.com/mongodb/mongo-tools-common/testtype"
	. "github.com/smartystreets/goconvey/convey"
)

// validateParseOptions is a helper function to call ParseOptions and verify the results.
// args: command line args
// expectSuccess: whether or not the error from ParseOptions should be nil
// ingestWc: the correct value for opts.IngestOptions.WriteConcern
// toolsWc: the correct value for opts.ToolsOptions.WriteConcern
func validateParseOptions(args []string, expectSuccess bool, ingestWc string, toolsWc *writeconcern.WriteConcern) func() {
	return func() {
		opts, err := ParseOptions(args, "", "")
		if expectSuccess {
			So(err, ShouldBeNil)
		} else {
			So(err, ShouldNotBeNil)
			return
		}

		So(opts.IngestOptions.WriteConcern, ShouldEqual, ingestWc)
		So(opts.ToolOptions.WriteConcern, ShouldResemble, toolsWc)
	}
}

// Regression test for TOOLS-1741
func TestWriteConcernWithURIParsing(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.UnitTestType)
	Convey("With an IngestOptions and ToolsOptions", t, func() {
		Convey("Parsing with no value should set a majority write concern",
			validateParseOptions([]string{}, true, "", writeconcern.New(writeconcern.WMajority())))

		Convey("Parsing with no writeconcern in URI should set a majority write concern",
			validateParseOptions([]string{
				"--uri", "mongodb://localhost:27017/test",
			}, true, "", writeconcern.New(writeconcern.WMajority())))

		Convey("Parsing with writeconcern only in URI should set it correctly",
			validateParseOptions([]string{
				"--uri", "mongodb://localhost:27017/test?w=2",
			}, true, "", writeconcern.New(writeconcern.W(2))))

		Convey("Parsing with writeconcern only in command line should set it correctly",
			validateParseOptions([]string{
				"--writeConcern", "{w: 2}",
			}, true, "{w: 2}", writeconcern.New(writeconcern.W(2))))

		Convey("Parsing with writeconcern in URI and command line should set to command line",
			validateParseOptions([]string{
				"--uri", "mongodb://localhost:27017/test?w=2",
				"--writeConcern", "{w: 3}",
			}, true, "{w: 3}", writeconcern.New(writeconcern.W(3))))
	})
}

// Test parsing for the --legacy flag
func TestLegacyOptionParsing(t *testing.T) {
	testCases := []struct {
		name           string
		legacyOpt      string // If "", --legacy will not be included as an option
		expectSuccess  bool
		expectedLegacy bool
	}{
		{"legacy defaults to false", "", true, false},
		{"legacy can be set", "true", true, true},
	}

	baseOpts := []string{"--host", "localhost:27017", "--db", "db", "--collection", "coll"}
	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			args := baseOpts
			if tc.legacyOpt != "" {
				args = append(args, "--legacy", tc.legacyOpt)
			}

			opts, err := ParseOptions(args, "", "")
			success := err == nil
			if success != tc.expectSuccess {
				t.Fatalf("expected err to be nil: %v; error was nil: %v", tc.expectSuccess, success)
			}
			if !tc.expectSuccess {
				return
			}

			if opts.Legacy != tc.expectedLegacy {
				t.Fatalf("legacy mismatch; expected %v, got %v", tc.expectedLegacy, opts.Legacy)
			}
		})
	}
}
