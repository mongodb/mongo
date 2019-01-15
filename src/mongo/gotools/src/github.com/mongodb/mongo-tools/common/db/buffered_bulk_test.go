// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	"os"
	"testing"

	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testtype"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
)

// Copied this var block, GetSSLOptions, and GetAuthOptions from common/testutil/auth.go to avoid circular dependency
var (
	UserAdmin              = "uAdmin"
	UserAdminPassword      = "password"
	CreatedUserNameEnv     = "TOOLS_TESTING_AUTH_USERNAME"
	CreatedUserPasswordEnv = "TOOLS_TESTING_AUTH_PASSWORD"
)

func DBGetSSLOptions() options.SSL {
	if testtype.HasTestType(testtype.SSLTestType) {
		return options.SSL{
			UseSSL:        true,
			SSLCAFile:     "./openssl/testdata/ca.pem",
			SSLPEMKeyFile: "./openssl/testdata/server.pem",
		}
	}

	return options.SSL{
		UseSSL: false,
	}
}

func DBGetAuthOptions() options.Auth {
	if testtype.HasTestType(testtype.AuthTestType) {
		return options.Auth{
			Username: os.Getenv(CreatedUserNameEnv),
			Password: os.Getenv(CreatedUserPasswordEnv),
			Source:   "admin",
		}
	}

	return options.Auth{}
}

func TestBufferedBulkInserterInserts(t *testing.T) {
	var bufBulk *BufferedBulkInserter

	testtype.SkipUnlessTestType(t, testtype.IntegrationTestType)

	auth := DBGetAuthOptions()
	ssl := DBGetSSLOptions()

	Convey("With a valid session", t, func() {
		opts := options.ToolOptions{
			Connection: &options.Connection{
				Port: DefaultTestPort,
			},
			SSL:  &ssl,
			Auth: &auth,
		}
		provider, err := NewSessionProvider(opts)
		So(provider, ShouldNotBeNil)
		So(err, ShouldBeNil)
		session, err := provider.GetSession()
		So(session, ShouldNotBeNil)
		So(err, ShouldBeNil)

		Convey("using a test collection and a doc limit of 3", func() {
			testCol := session.DB("tools-test").C("bulk1")
			bufBulk = NewBufferedBulkInserter(testCol, 3, false)
			So(bufBulk, ShouldNotBeNil)

			Convey("inserting 10 documents into the BufferedBulkInserter", func() {
				flushCount := 0
				for i := 0; i < 10; i++ {
					So(bufBulk.Insert(bson.D{}), ShouldBeNil)
					if bufBulk.docCount%3 == 0 {
						flushCount++
					}
				}

				Convey("should have flushed 3 times with one doc still buffered", func() {
					So(flushCount, ShouldEqual, 3)
					So(bufBulk.byteCount, ShouldBeGreaterThan, 0)
					So(bufBulk.docCount, ShouldEqual, 1)
				})
			})
		})

		Convey("using a test collection and a doc limit of 1", func() {
			testCol := session.DB("tools-test").C("bulk2")
			bufBulk = NewBufferedBulkInserter(testCol, 1, false)
			So(bufBulk, ShouldNotBeNil)

			Convey("inserting 10 documents into the BufferedBulkInserter and flushing", func() {
				for i := 0; i < 10; i++ {
					So(bufBulk.Insert(bson.D{}), ShouldBeNil)
				}
				So(bufBulk.Flush(), ShouldBeNil)

				Convey("should have no docs buffered", func() {
					So(bufBulk.docCount, ShouldEqual, 0)
					So(bufBulk.byteCount, ShouldEqual, 0)
				})
			})
		})

		Convey("using a test collection and a doc limit of 1000", func() {
			testCol := session.DB("tools-test").C("bulk3")
			bufBulk = NewBufferedBulkInserter(testCol, 100, false)
			So(bufBulk, ShouldNotBeNil)

			Convey("inserting 1,000,000 documents into the BufferedBulkInserter and flushing", func() {
				session.SetSocketTimeout(0)

				for i := 0; i < 1000000; i++ {
					bufBulk.Insert(bson.M{"_id": i})
				}
				So(bufBulk.Flush(), ShouldBeNil)

				Convey("should have inserted all of the documents", func() {
					count, err := testCol.Count()
					So(err, ShouldBeNil)
					So(count, ShouldEqual, 1000000)

					// test values
					testDoc := bson.M{}
					err = testCol.Find(bson.M{"_id": 477232}).One(&testDoc)
					So(err, ShouldBeNil)
					So(testDoc["_id"], ShouldEqual, 477232)
					err = testCol.Find(bson.M{"_id": 999999}).One(&testDoc)
					So(err, ShouldBeNil)
					So(testDoc["_id"], ShouldEqual, 999999)
					err = testCol.Find(bson.M{"_id": 1}).One(&testDoc)
					So(err, ShouldBeNil)
					So(testDoc["_id"], ShouldEqual, 1)

				})
			})
		})

		Reset(func() {
			session.DB("tools-test").DropDatabase()
			session.Close()
			provider.Close()
		})
	})

}
