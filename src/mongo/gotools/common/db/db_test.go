// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2"
	"reflect"
	"testing"
)

func TestNewSessionProvider(t *testing.T) {

	testutil.VerifyTestType(t, "db")

	Convey("When initializing a session provider", t, func() {

		Convey("with the standard options, a provider with a standard"+
			" connector should be returned", func() {
			opts := options.ToolOptions{
				Connection: &options.Connection{
					Port: DefaultTestPort,
				},
				SSL:  &options.SSL{},
				Auth: &options.Auth{},
			}
			provider, err := NewSessionProvider(opts)
			So(err, ShouldBeNil)
			So(reflect.TypeOf(provider.connector), ShouldEqual,
				reflect.TypeOf(&VanillaDBConnector{}))

			Convey("and should be closeable", func() {
				provider.Close()
			})

		})

		Convey("the master session should be successfully "+
			" initialized", func() {
			opts := options.ToolOptions{
				Connection: &options.Connection{
					Port: DefaultTestPort,
				},
				SSL:  &options.SSL{},
				Auth: &options.Auth{},
			}
			provider, err := NewSessionProvider(opts)
			So(err, ShouldBeNil)
			So(provider.masterSession, ShouldBeNil)
			session, err := provider.GetSession()
			So(err, ShouldBeNil)
			So(session, ShouldNotBeNil)
			session.Close()
			So(provider.masterSession, ShouldNotBeNil)
			err = provider.masterSession.Ping()
			So(err, ShouldBeNil)
			provider.Close()
			So(func() {
				provider.masterSession.Ping()
			}, ShouldPanic)

		})

	})

}

func TestGetIndexes(t *testing.T) {

	testutil.VerifyTestType(t, "db")

	Convey("With a valid session", t, func() {
		opts := options.ToolOptions{
			Connection: &options.Connection{
				Port: DefaultTestPort,
			},
			SSL:  &options.SSL{},
			Auth: &options.Auth{},
		}
		provider, err := NewSessionProvider(opts)
		So(err, ShouldBeNil)
		session, err := provider.GetSession()
		So(err, ShouldBeNil)

		existing := session.DB("exists").C("collection")
		missing := session.DB("exists").C("missing")
		missingDB := session.DB("missingDB").C("missingCollection")

		err = existing.Database.DropDatabase()
		So(err, ShouldBeNil)
		err = existing.Create(&mgo.CollectionInfo{})
		So(err, ShouldBeNil)
		err = missingDB.Database.DropDatabase()
		So(err, ShouldBeNil)

		Convey("When GetIndexes is called on", func() {
			Convey("an existing collection there should be no error", func() {
				indexesIter, err := GetIndexes(existing)
				So(err, ShouldBeNil)
				Convey("and indexes should be returned", func() {
					So(indexesIter, ShouldNotBeNil)
					var indexes []mgo.Index
					err := indexesIter.All(&indexes)
					So(err, ShouldBeNil)
					So(len(indexes), ShouldBeGreaterThan, 0)
				})
			})

			Convey("a missing collection there should be no error", func() {
				indexesIter, err := GetIndexes(missing)
				So(err, ShouldBeNil)
				Convey("and there should be no indexes", func() {
					So(indexesIter, ShouldBeNil)
				})
			})

			Convey("a missing database there should be no error", func() {
				indexesIter, err := GetIndexes(missingDB)
				So(err, ShouldBeNil)
				Convey("and there should be no indexes", func() {
					So(indexesIter, ShouldBeNil)
				})
			})
		})

		Reset(func() {
			existing.Database.DropDatabase()
			session.Close()
			provider.Close()
		})
	})
}

type listDatabasesCommand struct {
	Databases []map[string]interface{} `json:"databases"`
	Ok        bool                     `json:"ok"`
}

func (self *listDatabasesCommand) AsRunnable() interface{} {
	return "listDatabases"
}
