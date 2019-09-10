// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoexport

import (
	"testing"

	"go.mongodb.org/mongo-driver/mongo/readpref"
)

func TestParseOptions(t *testing.T) {
	t.Run("TestReadPreferenceParsing", func(t *testing.T) {
		// different sets of arguments to pass to ParseOptions
		secondaryCmdLine := []string{"--readPreference", "secondary"}
		slaveOkCmdLine := []string{"--slaveOk"}
		rpSlaveOkCmdLine := []string{"--slaveOk", "--readPreference", "secondary"}
		secondaryURI := []string{"--uri", "mongodb://localhost:27017/db?readPreference=secondary"}
		cmdLineAndURI := []string{"--uri", "mongodb://localhost:27017/db?readPreference=secondary", "--readPreference", "nearest"}

		testCases := []struct {
			name          string
			args          []string
			expectSuccess bool
			inputRp       string
			toolOptionsRp *readpref.ReadPref
		}{
			{"No values defaults to primary", []string{}, true, "", readpref.Primary()},
			{"Only command line", secondaryCmdLine, true, "secondary", readpref.Secondary()},
			{"Only URI", secondaryURI, true, "", readpref.Secondary()},
			{"Both URI and command line defaults to command line", cmdLineAndURI, true, "nearest", readpref.Nearest()},
			{"slaveOk becomes nearest", slaveOkCmdLine, true, "nearest", readpref.Nearest()},
			{"slaveOk and read pref errors", rpSlaveOkCmdLine, false, "", nil},
		}

		for _, tc := range testCases {
			t.Run(tc.name, func(t *testing.T) {
				opts, err := ParseOptions(tc.args, "", "")

				success := err == nil
				if success != tc.expectSuccess {
					t.Fatalf("expected err to be nil: %v; got error %v", tc.expectSuccess, err)
				}

				if !tc.expectSuccess {
					// Shouldn't compare read preferences if an error was expected
					return
				}

				if opts.InputOptions.ReadPreference != tc.inputRp {
					t.Fatalf("read preference mismatch on InputOptions; expected %v, got %v", tc.inputRp,
						opts.InputOptions.ReadPreference)
				}

				if tc.toolOptionsRp == nil {
					if opts.ToolOptions.ReadPreference != nil {
						t.Fatalf("expected read preference to be nil, got %v", opts.ToolOptions.ReadPreference)
					}
					return
				}

				expectedMode := tc.toolOptionsRp.Mode()
				gotMode := opts.ToolOptions.ReadPreference.Mode()
				if expectedMode != gotMode {
					t.Fatalf("read preference mode mismatch; expected %v, got %v", expectedMode, gotMode)
				}
			})
		}
	})

	t.Run("TestJSONFormat", func(t *testing.T) {
		testCases := []struct {
			name           string
			jsonFormat     JSONFormat // If "", no jsonFormat will be included
			expectSuccess  bool
			expectedFormat JSONFormat
		}{
			{"JSON format defaults to relaxed", "", true, Relaxed},
			{"JSON format can be set", Canonical, true, Canonical},
		}

		baseOpts := []string{"--host", "localhost:27017", "--db", "db", "--collection", "coll"}
		for _, tc := range testCases {
			t.Run(tc.name, func(t *testing.T) {
				args := baseOpts
				if tc.jsonFormat != "" {
					args = append(args, "--jsonFormat", string(tc.jsonFormat))
				}

				opts, err := ParseOptions(args, "", "")
				success := err == nil
				if success != tc.expectSuccess {
					t.Fatalf("expected err to be nil: %v; error was nil: %v", tc.expectSuccess, success)
				}
				if !tc.expectSuccess {
					return
				}

				if opts.JSONFormat != tc.expectedFormat {
					t.Fatalf("JSON format mismatch; expected %v, got %v", tc.expectedFormat, opts.JSONFormat)
				}
			})
		}
	})
}
