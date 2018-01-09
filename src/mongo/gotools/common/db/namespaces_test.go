// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	"fmt"
	"testing"

	. "github.com/smartystreets/goconvey/convey"
)

type stripDBFromNamespaceTestCase struct {
	inputNamespace string
	inputDBName    string

	outputNamespace string
	outputError     error
}

func TestStripDBFromNamespace(t *testing.T) {
	Convey("When testing StripDBFromNamespace with cases", t, func() {
		testCases := []stripDBFromNamespaceTestCase{
			{
				inputNamespace: "database.col",
				inputDBName:    "database",

				outputNamespace: "col",
				outputError:     nil,
			},
			{
				inputNamespace: "database2.col",
				inputDBName:    "database",

				outputNamespace: "",
				outputError:     fmt.Errorf("namespace 'database2.col' format is invalid - expected to start with 'database.'"),
			},
			{
				inputNamespace: "database.col",
				inputDBName:    "notAPrefix",

				outputNamespace: "",
				outputError:     fmt.Errorf("namespace 'database.col' format is invalid - expected to start with 'notAPrefix.'"),
			},
		}
		Convey("cases should match expected", func() {
			for _, tc := range testCases {
				resultNamespace, resultError := StripDBFromNamespace(tc.inputNamespace, tc.inputDBName)
				So(resultError, ShouldResemble, tc.outputError)
				So(resultNamespace, ShouldEqual, tc.outputNamespace)
			}
		})
	})

}
