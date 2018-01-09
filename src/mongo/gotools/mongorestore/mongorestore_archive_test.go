// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

import (
	"github.com/mongodb/mongo-tools/common/archive"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"

	. "github.com/smartystreets/goconvey/convey"

	"io"
	"io/ioutil"
	"os"
	"testing"
)

func init() {
	// bump up the verbosity to make checking debug log output possible
	log.SetVerbosity(&options.Verbosity{
		VLevel: 4,
	})
}

var (
	testArchive = "testdata/test.bar.archive"
)

func TestMongorestoreShortArchive(t *testing.T) {
	testutil.VerifyTestType(t, testutil.IntegrationTestType)
	_, err := testutil.GetBareSession()
	if err != nil {
		t.Fatalf("No server available")
	}

	Convey("With a test MongoRestore", t, func() {
		inputOptions := &InputOptions{
			Archive: testArchive,
		}
		outputOptions := &OutputOptions{
			NumParallelCollections: 1,
			NumInsertionWorkers:    1,
			Drop:                   true,
		}
		nsOptions := &NSOptions{}
		provider, toolOpts, err := testutil.GetBareSessionProvider()
		file, err := os.Open(testArchive)
		So(file, ShouldNotBeNil)
		So(err, ShouldBeNil)

		fi, err := file.Stat()
		So(fi, ShouldNotBeNil)
		So(err, ShouldBeNil)

		fileSize := fi.Size()

		for i := fileSize; i >= 0; i-- {

			log.Logvf(log.Always, "Restoring from the first %v bytes of a archive of size %v", i, fileSize)

			_, err = file.Seek(0, 0)
			So(err, ShouldBeNil)

			restore := MongoRestore{
				ToolOptions:     toolOpts,
				OutputOptions:   outputOptions,
				InputOptions:    inputOptions,
				NSOptions:       nsOptions,
				SessionProvider: provider,
				archive: &archive.Reader{
					Prelude: &archive.Prelude{},
					In:      ioutil.NopCloser(io.LimitReader(file, i)),
				},
			}
			err = restore.Restore()
			if i == fileSize {
				So(err, ShouldBeNil)
			} else {
				So(err, ShouldNotBeNil)
			}
		}
	})
}
