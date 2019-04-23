// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongodump

import (
	"testing"

	"github.com/mongodb/mongo-tools-common/testtype"
	. "github.com/smartystreets/goconvey/convey"
)

func TestSkipCollection(t *testing.T) {

	testtype.SkipUnlessTestType(t, testtype.UnitTestType)

	Convey("With a mongodump that excludes collections 'test' and 'fake'"+
		" and excludes prefixes 'pre-' and 'no'", t, func() {
		md := &MongoDump{
			OutputOptions: &OutputOptions{
				ExcludedCollections:        []string{"test", "fake"},
				ExcludedCollectionPrefixes: []string{"pre-", "no"},
			},
		}

		Convey("collection 'pre-test' should be skipped", func() {
			So(md.shouldSkipCollection("pre-test"), ShouldBeTrue)
		})

		Convey("collection 'notest' should be skipped", func() {
			So(md.shouldSkipCollection("notest"), ShouldBeTrue)
		})

		Convey("collection 'test' should be skipped", func() {
			So(md.shouldSkipCollection("test"), ShouldBeTrue)
		})

		Convey("collection 'fake' should be skipped", func() {
			So(md.shouldSkipCollection("fake"), ShouldBeTrue)
		})

		Convey("collection 'fake222' should not be skipped", func() {
			So(md.shouldSkipCollection("fake222"), ShouldBeFalse)
		})

		Convey("collection 'random' should not be skipped", func() {
			So(md.shouldSkipCollection("random"), ShouldBeFalse)
		})

		Convey("collection 'mytest' should not be skipped", func() {
			So(md.shouldSkipCollection("mytest"), ShouldBeFalse)
		})
	})

}

type testTable struct {
	db     string
	coll   string
	output bool
}

func TestShouldSkipSystemNamespace(t *testing.T) {
	testtype.SkipUnlessTestType(t, testtype.UnitTestType)
	tests := []testTable{
		{
			db:     "test",
			coll:   "system",
			output: false,
		},
		{
			db:     "test",
			coll:   "system.nonsense",
			output: true,
		},
		{
			db:     "test",
			coll:   "system.js",
			output: false,
		},
		{
			db:     "test",
			coll:   "test",
			output: false,
		},
	}

	for _, testVals := range tests {
		if shouldSkipSystemNamespace(testVals.db, testVals.coll) != testVals.output {
			t.Errorf("%s.%s should have been %v but failed\n", testVals.db, testVals.coll, testVals.output)
		}
	}
}
