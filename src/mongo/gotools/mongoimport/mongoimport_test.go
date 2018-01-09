// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoimport

import (
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"reflect"
	"runtime"
	"strings"
	"testing"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
)

const (
	testDb         = "db"
	testCollection = "c"
)

// checkOnlyHasDocuments returns an error if the documents in the test
// collection don't exactly match those that are passed in
func checkOnlyHasDocuments(sessionProvider db.SessionProvider, expectedDocuments []bson.M) error {
	session, err := sessionProvider.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()

	collection := session.DB(testDb).C(testCollection)
	dbDocuments := []bson.M{}
	err = collection.Find(nil).Sort("_id").All(&dbDocuments)
	if err != nil {
		return err
	}
	if len(dbDocuments) != len(expectedDocuments) {
		return fmt.Errorf("document count mismatch: expected %#v, got %#v",
			len(expectedDocuments), len(dbDocuments))
	}
	for index := range dbDocuments {
		if !reflect.DeepEqual(dbDocuments[index], expectedDocuments[index]) {
			return fmt.Errorf("document mismatch: expected %#v, got %#v",
				expectedDocuments[index], dbDocuments[index])
		}
	}
	return nil
}

// getBasicToolOptions returns a test helper to instantiate the session provider
// for calls to StreamDocument
func getBasicToolOptions() *options.ToolOptions {
	general := &options.General{}
	ssl := testutil.GetSSLOptions()
	auth := testutil.GetAuthOptions()
	namespace := &options.Namespace{
		DB:         testDb,
		Collection: testCollection,
	}
	connection := &options.Connection{
		Host: "localhost",
		Port: db.DefaultTestPort,
	}

	return &options.ToolOptions{
		General:    general,
		SSL:        &ssl,
		Namespace:  namespace,
		Connection: connection,
		Auth:       &auth,
		URI:        &options.URI{},
	}
}

func NewMongoImport() (*MongoImport, error) {
	toolOptions := getBasicToolOptions()
	inputOptions := &InputOptions{
		ParseGrace: "stop",
	}
	ingestOptions := &IngestOptions{}
	provider, err := db.NewSessionProvider(*toolOptions)
	if err != nil {
		return nil, err
	}
	return &MongoImport{
		ToolOptions:     toolOptions,
		InputOptions:    inputOptions,
		IngestOptions:   ingestOptions,
		SessionProvider: provider,
	}, nil
}

func TestSplitInlineHeader(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("handle normal, untyped headers", t, func() {
		fields := []string{"foo.bar", "baz", "boo"}
		header := strings.Join(fields, ",")
		Convey("with '"+header+"'", func() {
			So(splitInlineHeader(header), ShouldResemble, fields)
		})
	})
	Convey("handle typed headers", t, func() {
		fields := []string{"foo.bar.string()", "baz.date(January 2 2006)", "boo.binary(hex)"}
		header := strings.Join(fields, ",")
		Convey("with '"+header+"'", func() {
			So(splitInlineHeader(header), ShouldResemble, fields)
		})
	})
	Convey("handle typed headers that include commas", t, func() {
		fields := []string{"foo.bar.date(,,,,)", "baz.date(January 2, 2006)", "boo.binary(hex)"}
		header := strings.Join(fields, ",")
		Convey("with '"+header+"'", func() {
			So(splitInlineHeader(header), ShouldResemble, fields)
		})
	})
}

func TestMongoImportValidateSettings(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Given a mongoimport instance for validation, ", t, func() {
		Convey("an error should be thrown if no collection is given", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.ToolOptions.Namespace.DB = ""
			imp.ToolOptions.Namespace.Collection = ""
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if an invalid type is given", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = "invalid"
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if neither --headerline is supplied "+
			"nor --fields/--fieldFile", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("no error should be thrown if --headerline is not supplied "+
			"but --fields is supplied", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fields := "a,b,c"
			imp.InputOptions.Fields = &fields
			imp.InputOptions.Type = CSV
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("no error should be thrown if no input type is supplied", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("no error should be thrown if there's just one positional argument", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			So(imp.ValidateSettings([]string{"a"}), ShouldBeNil)
		})

		Convey("an error should be thrown if --file is used with one positional argument", func() {
			imp, err := NewMongoImport()
			imp.InputOptions.File = "abc"
			So(err, ShouldBeNil)
			So(imp.ValidateSettings([]string{"a"}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if there's more than one positional argument", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			So(imp.ValidateSettings([]string{"a", "b"}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if --headerline is used with JSON input", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.HeaderLine = true
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if --fields is used with JSON input", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fields := ""
			imp.InputOptions.Fields = &fields
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
			fields = "a,b,c"
			imp.InputOptions.Fields = &fields
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if --fieldFile is used with JSON input", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := ""
			imp.InputOptions.FieldFile = &fieldFile
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
			fieldFile = "test.csv"
			imp.InputOptions.FieldFile = &fieldFile
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if --ignoreBlanks is used with JSON input", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.IngestOptions.IgnoreBlanks = true
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("no error should be thrown if --headerline is not supplied "+
			"but --fieldFile is supplied", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "test.csv"
			imp.InputOptions.FieldFile = &fieldFile
			imp.InputOptions.Type = CSV
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("an error should be thrown if --mode is incorrect", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.IngestOptions.Mode = "wrong"
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if a field in the --upsertFields "+
			"argument starts with a dollar sign", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.HeaderLine = true
			imp.InputOptions.Type = CSV
			imp.IngestOptions.Mode = modeUpsert
			imp.IngestOptions.UpsertFields = "a,$b,c"
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
			imp.IngestOptions.UpsertFields = "a,.b,c"
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("no error should be thrown if --upsertFields is supplied without "+
			"--mode=xxx", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.HeaderLine = true
			imp.InputOptions.Type = CSV
			imp.IngestOptions.UpsertFields = "a,b,c"
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
			So(imp.IngestOptions.Mode, ShouldEqual, modeUpsert)
		})

		Convey("an error should be thrown if --upsertFields is used with "+
			"--mode=insert", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.HeaderLine = true
			imp.InputOptions.Type = CSV
			imp.IngestOptions.Mode = modeInsert
			imp.IngestOptions.UpsertFields = "a"
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("if --mode=upsert is used without --upsertFields, _id should be set as "+
			"the upsert field", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.HeaderLine = true
			imp.InputOptions.Type = CSV
			imp.IngestOptions.Mode = modeUpsert
			imp.IngestOptions.UpsertFields = ""
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
			So(imp.upsertFields, ShouldResemble, []string{"_id"})
		})

		Convey("no error should be thrown if all fields in the --upsertFields "+
			"argument are valid", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.HeaderLine = true
			imp.InputOptions.Type = CSV
			imp.IngestOptions.Mode = modeUpsert
			imp.IngestOptions.UpsertFields = "a,b,c"
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("no error should be thrown if --fields is supplied with CSV import", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fields := "a,b,c"
			imp.InputOptions.Fields = &fields
			imp.InputOptions.Type = CSV
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("an error should be thrown if an empty --fields is supplied with CSV import", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fields := ""
			imp.InputOptions.Fields = &fields
			imp.InputOptions.Type = CSV
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("no error should be thrown if --fieldFile is supplied with CSV import", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "test.csv"
			imp.InputOptions.FieldFile = &fieldFile
			imp.InputOptions.Type = CSV
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("an error should be thrown if no collection and no file is supplied", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "test.csv"
			imp.InputOptions.FieldFile = &fieldFile
			imp.InputOptions.Type = CSV
			imp.ToolOptions.Namespace.Collection = ""
			So(imp.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("no error should be thrown if --file is used (without -c) supplied "+
			"- the file name should be used as the collection name", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.File = "input"
			imp.InputOptions.HeaderLine = true
			imp.InputOptions.Type = CSV
			imp.ToolOptions.Namespace.Collection = ""
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
			So(imp.ToolOptions.Namespace.Collection, ShouldEqual,
				imp.InputOptions.File)
		})

		Convey("with no collection name and a file name the base name of the "+
			"file (without the extension) should be used as the collection name", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.File = "/path/to/input/file/dot/input.txt"
			imp.InputOptions.HeaderLine = true
			imp.InputOptions.Type = CSV
			imp.ToolOptions.Namespace.Collection = ""
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
			So(imp.ToolOptions.Namespace.Collection, ShouldEqual, "input")
		})
	})
}

func TestGetSourceReader(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("Given a mongoimport instance, on calling getSourceReader", t,
		func() {
			Convey("an error should be thrown if the given file referenced by "+
				"the reader does not exist", func() {
				imp, err := NewMongoImport()
				So(err, ShouldBeNil)
				imp.InputOptions.File = "/path/to/input/file/dot/input.txt"
				imp.InputOptions.Type = CSV
				imp.ToolOptions.Namespace.Collection = ""
				_, _, err = imp.getSourceReader()
				So(err, ShouldNotBeNil)
			})

			Convey("no error should be thrown if the file exists", func() {
				imp, err := NewMongoImport()
				So(err, ShouldBeNil)
				imp.InputOptions.File = "testdata/test_array.json"
				imp.InputOptions.Type = JSON
				_, _, err = imp.getSourceReader()
				So(err, ShouldBeNil)
			})

			Convey("no error should be thrown if stdin is used", func() {
				imp, err := NewMongoImport()
				So(err, ShouldBeNil)
				imp.InputOptions.File = ""
				_, _, err = imp.getSourceReader()
				So(err, ShouldBeNil)
			})
		})
}

func TestGetInputReader(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("Given a io.Reader on calling getInputReader", t, func() {
		Convey("should parse --fields using valid csv escaping", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Fields = new(string)
			*imp.InputOptions.Fields = "foo.auto(),bar.date(January 2, 2006)"
			imp.InputOptions.File = "/path/to/input/file/dot/input.txt"
			imp.InputOptions.ColumnsHaveTypes = true
			_, err = imp.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("should complain about non-escaped new lines in --fields", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Fields = new(string)
			*imp.InputOptions.Fields = "foo.auto(),\nblah.binary(hex),bar.date(January 2, 2006)"
			imp.InputOptions.File = "/path/to/input/file/dot/input.txt"
			imp.InputOptions.ColumnsHaveTypes = true
			_, err = imp.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown if neither --fields nor --fieldFile "+
			"is used", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.File = "/path/to/input/file/dot/input.txt"
			_, err = imp.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown if --fields is used", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fields := "a,b,c"
			imp.InputOptions.Fields = &fields
			imp.InputOptions.File = "/path/to/input/file/dot/input.txt"
			_, err = imp.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown if --fieldFile is used and it "+
			"references a valid file", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "testdata/test.csv"
			imp.InputOptions.FieldFile = &fieldFile
			_, err = imp.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("an error should be thrown if --fieldFile is used and it "+
			"references an invalid file", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "/path/to/input/file/dot/input.txt"
			imp.InputOptions.FieldFile = &fieldFile
			_, err = imp.getInputReader(&os.File{})
			So(err, ShouldNotBeNil)
		})
		Convey("no error should be thrown for CSV import inputs", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			_, err = imp.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown for TSV import inputs", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = TSV
			_, err = imp.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown for JSON import inputs", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = JSON
			_, err = imp.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("an error should be thrown if --fieldFile fields are invalid", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "testdata/test_fields_invalid.txt"
			imp.InputOptions.FieldFile = &fieldFile
			file, err := os.Open(fieldFile)
			So(err, ShouldBeNil)
			_, err = imp.getInputReader(file)
			So(err, ShouldNotBeNil)
		})
		Convey("no error should be thrown if --fieldFile fields are valid", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "testdata/test_fields_valid.txt"
			imp.InputOptions.FieldFile = &fieldFile
			file, err := os.Open(fieldFile)
			So(err, ShouldBeNil)
			_, err = imp.getInputReader(file)
			So(err, ShouldBeNil)
		})
	})
}

func TestImportDocuments(t *testing.T) {
	testutil.VerifyTestType(t, testutil.IntegrationTestType)
	Convey("With a mongoimport instance", t, func() {
		Reset(func() {
			sessionProvider, err := db.NewSessionProvider(*getBasicToolOptions())
			if err != nil {
				t.Fatalf("error getting session provider session: %v", err)
			}
			session, err := sessionProvider.GetSession()
			if err != nil {
				t.Fatalf("error getting session: %v", err)
			}
			defer session.Close()
			session.DB(testDb).C(testCollection).DropCollection()
		})
		Convey("no error should be thrown for CSV import on test data and all "+
			"CSV data lines should be imported correctly", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test.csv"
			fields := "a,b,c"
			imp.InputOptions.Fields = &fields
			imp.IngestOptions.WriteConcern = "majority"
			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
		})
		Convey("an error should be thrown for JSON import on test data that is "+
			"JSON array", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.File = "testdata/test_array.json"
			imp.IngestOptions.WriteConcern = "majority"
			numImported, err := imp.ImportDocuments()
			So(err, ShouldNotBeNil)
			So(numImported, ShouldEqual, 0)
		})
		Convey("TOOLS-247: no error should be thrown for JSON import on test "+
			"data and all documents should be imported correctly", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.File = "testdata/test_plain2.json"
			imp.IngestOptions.WriteConcern = "majority"
			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 10)
		})
		Convey("CSV import with --ignoreBlanks should import only non-blank fields", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test_blanks.csv"
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			imp.IngestOptions.IgnoreBlanks = true

			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": int(2)},
				bson.M{"_id": 5, "c": "6e"},
				bson.M{"_id": 7, "b": int(8), "c": int(6)},
			}
			So(checkOnlyHasDocuments(*imp.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("CSV import without --ignoreBlanks should include blanks", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test_blanks.csv"
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": int(2), "c": ""},
				bson.M{"_id": 5, "b": "", "c": "6e"},
				bson.M{"_id": 7, "b": int(8), "c": int(6)},
			}
			So(checkOnlyHasDocuments(*imp.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("no error should be thrown for CSV import on test data with --upsertFields", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test.csv"
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			imp.IngestOptions.UpsertFields = "b,c"
			imp.IngestOptions.MaintainInsertionOrder = true
			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": int(2), "c": int(3)},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": int(6), "c": int(6)},
			}
			So(checkOnlyHasDocuments(*imp.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("no error should be thrown for CSV import on test data with "+
			"--stopOnError. Only documents before error should be imported", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test.csv"
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			imp.IngestOptions.StopOnError = true
			imp.IngestOptions.MaintainInsertionOrder = true
			imp.IngestOptions.WriteConcern = "majority"
			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": int(2), "c": int(3)},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": int(6), "c": int(6)},
			}
			So(checkOnlyHasDocuments(*imp.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("CSV import with duplicate _id's should not error if --stopOnError is not set", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)

			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test_duplicate.csv"
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			imp.IngestOptions.StopOnError = false
			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 5)

			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": int(2), "c": int(3)},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": int(6), "c": int(6)},
				bson.M{"_id": 8, "b": int(6), "c": int(6)},
			}
			// all docs except the one with duplicate _id - should be imported
			So(checkOnlyHasDocuments(*imp.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("no error should be thrown for CSV import on test data with --drop", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test.csv"
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			imp.IngestOptions.Drop = true
			imp.IngestOptions.MaintainInsertionOrder = true
			imp.IngestOptions.WriteConcern = "majority"
			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": int(2), "c": int(3)},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": int(6), "c": int(6)},
			}
			So(checkOnlyHasDocuments(*imp.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("CSV import on test data with --headerLine should succeed", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test.csv"
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			imp.InputOptions.HeaderLine = true
			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 2)
		})
		Convey("EOF should be thrown for CSV import with --headerLine if file is empty", func() {
			csvFile, err := ioutil.TempFile("", "mongoimport_")
			So(err, ShouldBeNil)
			csvFile.Close()

			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = csvFile.Name()
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			imp.InputOptions.HeaderLine = true
			numImported, err := imp.ImportDocuments()
			So(err, ShouldEqual, io.EOF)
			So(numImported, ShouldEqual, 0)
		})
		Convey("CSV import with --mode=upsert and --upsertFields should succeed", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)

			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test.csv"
			fields := "_id,c,b"
			imp.InputOptions.Fields = &fields
			imp.IngestOptions.UpsertFields = "_id"
			imp.IngestOptions.MaintainInsertionOrder = true
			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "c": int(2), "b": int(3)},
				bson.M{"_id": 3, "c": 5.4, "b": "string"},
				bson.M{"_id": 5, "c": int(6), "b": int(6)},
			}
			So(checkOnlyHasDocuments(*imp.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("CSV import with --mode=upsert/--upsertFields with duplicate id should succeed "+
			"if stopOnError is not set", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test_duplicate.csv"
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			imp.IngestOptions.Mode = modeUpsert
			imp.upsertFields = []string{"_id"}
			numImported, err := imp.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 5)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": int(2), "c": int(3)},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": int(6), "c": int(9)},
				bson.M{"_id": 8, "b": int(6), "c": int(6)},
			}
			So(checkOnlyHasDocuments(*imp.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("an error should be thrown for CSV import on test data with "+
			"duplicate _id if --stopOnError is set", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test_duplicate.csv"
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			imp.IngestOptions.StopOnError = true
			imp.IngestOptions.WriteConcern = "1"
			imp.IngestOptions.MaintainInsertionOrder = true
			_, err = imp.ImportDocuments()
			So(err, ShouldNotBeNil)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": int(2), "c": int(3)},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": int(6), "c": int(6)},
			}
			So(checkOnlyHasDocuments(*imp.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("an error should be thrown for JSON import on test data that "+
			"is a JSON array without passing --jsonArray", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.File = "testdata/test_array.json"
			imp.IngestOptions.WriteConcern = "1"
			_, err = imp.ImportDocuments()
			So(err, ShouldNotBeNil)
		})
		Convey("an error should be thrown if a plain JSON file is supplied", func() {
			fileHandle, err := os.Open("testdata/test_plain.json")
			So(err, ShouldBeNil)
			jsonInputReader := NewJSONInputReader(true, fileHandle, 1)
			docChan := make(chan bson.D, 1)
			So(jsonInputReader.StreamDocument(true, docChan), ShouldNotBeNil)
		})
		Convey("an error should be thrown for invalid CSV import on test data", func() {
			imp, err := NewMongoImport()
			So(err, ShouldBeNil)
			imp.InputOptions.Type = CSV
			imp.InputOptions.File = "testdata/test_bad.csv"
			fields := "_id,b,c"
			imp.InputOptions.Fields = &fields
			imp.IngestOptions.StopOnError = true
			imp.IngestOptions.WriteConcern = "1"
			imp.IngestOptions.MaintainInsertionOrder = true
			_, err = imp.ImportDocuments()
			So(err, ShouldNotBeNil)
		})
	})
}

// Regression test for TOOLS-1694 to prevent issue from TOOLS-1115
func TestHiddenOptionsDefaults(t *testing.T) {
	Convey("With a new mongoimport with empty options", t, func() {
		imp, err := NewMongoImport()
		imp.ToolOptions = options.New("", "", options.EnabledOptions{})
		So(err, ShouldBeNil)
		Convey("Then parsing should fill args with expected defaults", func() {
			_, err := imp.ToolOptions.ParseArgs([]string{})
			So(err, ShouldBeNil)

			// collection cannot be empty in validate
			imp.ToolOptions.Collection = "col"
			So(imp.ValidateSettings([]string{}), ShouldBeNil)
			So(imp.IngestOptions.NumDecodingWorkers, ShouldEqual, runtime.NumCPU())
			So(imp.IngestOptions.BulkBufferSize, ShouldEqual, 1000)
		})
	})

}
