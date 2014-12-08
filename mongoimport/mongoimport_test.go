package mongoimport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	"github.com/mongodb/mongo-tools/mongoimport/options"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"io"
	"io/ioutil"
	"os"
	"reflect"
	"testing"
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
func getBasicToolOptions() *commonOpts.ToolOptions {
	ssl := testutil.GetSSLOptions()
	auth := testutil.GetAuthOptions()
	namespace := &commonOpts.Namespace{
		DB:         testDb,
		Collection: testCollection,
	}
	connection := &commonOpts.Connection{
		Host: "localhost",
		Port: "27017",
	}
	return &commonOpts.ToolOptions{
		SSL:           &ssl,
		Namespace:     namespace,
		Connection:    connection,
		HiddenOptions: &commonOpts.HiddenOptions{},
		Auth:          &auth,
	}
}

func NewMongoImport() (*MongoImport, error) {
	toolOptions := getBasicToolOptions()
	inputOptions := &options.InputOptions{}
	ingestOptions := &options.IngestOptions{}
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

func TestMongoImportValidateSettings(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given a mongoimport instance for validation, ", t, func() {
		Convey("an error should be thrown if no collection is given", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.ToolOptions.Namespace.DB = ""
			mongoImport.ToolOptions.Namespace.Collection = ""
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if an invalid type is given", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = "invalid"
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if neither --headerline is supplied "+
			"nor --fields/--fieldFile", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("no error should be thrown if --headerline is not supplied "+
			"but --fields is supplied", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			fields := "a,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.InputOptions.Type = CSV
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("no error should be thrown if no input type is supplied", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("an error should be thrown if --headerline is used with JSON input", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.HeaderLine = true
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if --fields is used with JSON input", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			fields := ""
			mongoImport.InputOptions.Fields = &fields
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
			fields = "a,b,c"
			mongoImport.InputOptions.Fields = &fields
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if --fieldFile is used with JSON input", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := ""
			mongoImport.InputOptions.FieldFile = &fieldFile
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
			fieldFile = "test.csv"
			mongoImport.InputOptions.FieldFile = &fieldFile
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("no error should be thrown if --headerline is not supplied "+
			"but --fieldFile is supplied", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "test.csv"
			mongoImport.InputOptions.FieldFile = &fieldFile
			mongoImport.InputOptions.Type = CSV
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("no error should be thrown if --fields is supplied with CSV import", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			fields := "a,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.InputOptions.Type = CSV
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
			fields = ""
			mongoImport.InputOptions.Fields = &fields
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("an error should be thrown if empty --fieldFile is used with CSV input", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := ""
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.FieldFile = &fieldFile
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if no collection and no file is supplied", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "test.csv"
			mongoImport.InputOptions.FieldFile = &fieldFile
			mongoImport.InputOptions.Type = CSV
			mongoImport.ToolOptions.Namespace.Collection = ""
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("no error should be thrown if no collection but a file is "+
			"supplied and the file name should be used as the collection name", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.File = "input"
			fieldFile := "test.csv"
			mongoImport.InputOptions.FieldFile = &fieldFile
			mongoImport.InputOptions.Type = CSV
			mongoImport.ToolOptions.Namespace.Collection = ""
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
			So(mongoImport.ToolOptions.Namespace.Collection, ShouldEqual,
				mongoImport.InputOptions.File)
		})

		Convey("with no collection name and a file name the base name of the "+
			"file (without the extension) should be used as the collection name", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.File = "/path/to/input/file/dot/input.txt"
			fieldFile := "test.csv"
			mongoImport.InputOptions.FieldFile = &fieldFile
			mongoImport.InputOptions.Type = CSV
			mongoImport.ToolOptions.Namespace.Collection = ""
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
			So(mongoImport.ToolOptions.Namespace.Collection, ShouldEqual, "input")
		})
	})
}

func TestGetSourceReader(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)
	Convey("Given a mongoimport instance, on calling getSourceReader", t,
		func() {
			Convey("an error should be thrown if the given file referenced by "+
				"the reader does not exist", func() {
				mongoImport, err := NewMongoImport()
				So(err, ShouldBeNil)
				mongoImport.InputOptions.File = "/path/to/input/file/dot/input.txt"
				mongoImport.InputOptions.Type = CSV
				mongoImport.ToolOptions.Namespace.Collection = ""
				_, err = mongoImport.getSourceReader()
				So(err, ShouldNotBeNil)
			})

			Convey("no error should be thrown if the file exists", func() {
				mongoImport, err := NewMongoImport()
				So(err, ShouldBeNil)
				mongoImport.InputOptions.File = "testdata/test_array.json"
				mongoImport.InputOptions.Type = JSON
				_, err = mongoImport.getSourceReader()
				So(err, ShouldBeNil)
			})

			Convey("no error should be thrown if stdin is used", func() {
				mongoImport, err := NewMongoImport()
				So(err, ShouldBeNil)
				mongoImport.InputOptions.File = ""
				_, err = mongoImport.getSourceReader()
				So(err, ShouldBeNil)
			})
		})
}

func TestGetInputReader(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)
	Convey("Given a io.Reader on calling getInputReader", t, func() {
		Convey("no error should be thrown if neither --fields nor --fieldFile "+
			"is used", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.File = "/path/to/input/file/dot/input.txt"
			_, err = mongoImport.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown if --fields is used", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			fields := "a,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.InputOptions.File = "/path/to/input/file/dot/input.txt"
			_, err = mongoImport.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown if --fieldFile is used and it "+
			"references a valid file", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "testdata/test_array.json"
			mongoImport.InputOptions.FieldFile = &fieldFile
			_, err = mongoImport.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("an error should be thrown if --fieldFile is used and it "+
			"references an invalid file", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			fieldFile := "/path/to/input/file/dot/input.txt"
			mongoImport.InputOptions.FieldFile = &fieldFile
			_, err = mongoImport.getInputReader(&os.File{})
			So(err, ShouldNotBeNil)
		})
		Convey("no error should be thrown for CSV import inputs", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			_, err = mongoImport.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown for TSV import inputs", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = TSV
			_, err = mongoImport.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown for JSON import inputs", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = JSON
			_, err = mongoImport.getInputReader(&os.File{})
			So(err, ShouldBeNil)
		})
	})
}

func TestImportDocuments(t *testing.T) {
	testutil.VerifyTestType(t, testutil.INTEGRATION_TEST_TYPE)
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
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test.csv"
			fields := "a,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.IngestOptions.WriteConcern = "majority"
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
		})
		Convey("an error should be thrown for JSON import on test data that is "+
			"JSON array", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.File = "testdata/test_array.json"
			mongoImport.IngestOptions.WriteConcern = "majority"
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldNotBeNil)
			So(numImported, ShouldEqual, 0)
		})
		Convey("TOOLS-247: no error should be thrown for JSON import on test "+
			"data and all documents should be imported correctly", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.File = "testdata/test_plain2.json"
			mongoImport.IngestOptions.WriteConcern = "majority"
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 10)
		})
		Convey("CSV import with --ignoreBlanks should import only non-blank fields", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test_blanks.csv"
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.IngestOptions.IgnoreBlanks = true

			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2},
				bson.M{"_id": 5, "c": "6e"},
				bson.M{"_id": 7, "b": 8, "c": 6},
			}
			So(checkOnlyHasDocuments(*mongoImport.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("CSV import without --ignoreBlanks should include blanks", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test_blanks.csv"
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": ""},
				bson.M{"_id": 5, "b": "", "c": "6e"},
				bson.M{"_id": 7, "b": 8, "c": 6},
			}
			So(checkOnlyHasDocuments(*mongoImport.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("no error should be thrown for CSV import on test data with --upsert", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test.csv"
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.IngestOptions.Upsert = true
			mongoImport.IngestOptions.MaintainInsertionOrder = true
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(*mongoImport.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("no error should be thrown for CSV import on test data with "+
			"--stopOnError. Only documents before error should be imported", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test.csv"
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.IngestOptions.StopOnError = true
			mongoImport.IngestOptions.MaintainInsertionOrder = true
			mongoImport.IngestOptions.WriteConcern = "majority"
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(*mongoImport.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("CSV import with duplicate _id's should not error if --stopOnError is not set", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)

			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test_duplicate.csv"
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.IngestOptions.StopOnError = false
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 5)

			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 6},
				bson.M{"_id": 8, "b": 6, "c": 6},
			}
			// all docs except the one with duplicate _id - should be imported
			So(checkOnlyHasDocuments(*mongoImport.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("no error should be thrown for CSV import on test data with --drop", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test.csv"
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.IngestOptions.Drop = true
			mongoImport.IngestOptions.MaintainInsertionOrder = true
			mongoImport.IngestOptions.WriteConcern = "majority"
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(*mongoImport.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("CSV import on test data with --headerLine should succeed", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test.csv"
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.InputOptions.HeaderLine = true
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 2)
		})
		Convey("EOF should be thrown for CSV import with --headerLine if file is empty", func() {
			csvFile, err := ioutil.TempFile("", "mongoimport_")
			So(err, ShouldBeNil)
			csvFile.Close()

			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = csvFile.Name()
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.InputOptions.HeaderLine = true
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldEqual, io.EOF)
			So(numImported, ShouldEqual, 0)
		})
		Convey("CSV import with --upsert and --upsertFields should succeed", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)

			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test.csv"
			fields := "_id,c,b"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.IngestOptions.Upsert = true
			mongoImport.IngestOptions.UpsertFields = "_id"
			mongoImport.IngestOptions.MaintainInsertionOrder = true
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "c": 2, "b": 3},
				bson.M{"_id": 3, "c": 5.4, "b": "string"},
				bson.M{"_id": 5, "c": 6, "b": 6},
			}
			So(checkOnlyHasDocuments(*mongoImport.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("CSV import with --upsert/--upsertFields with duplicate id should succeed"+
			" if stopOnError is not set", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test_duplicate.csv"
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.IngestOptions.Upsert = true
			mongoImport.IngestOptions.UpsertFields = "_id"
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 5)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 9},
				bson.M{"_id": 8, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(*mongoImport.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("an error should be thrown for CSV import on test data with "+
			"duplicate _id if --stopOnError is set", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test_duplicate.csv"
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.IngestOptions.StopOnError = true
			mongoImport.IngestOptions.WriteConcern = "1"
			mongoImport.IngestOptions.MaintainInsertionOrder = true
			_, err = mongoImport.ImportDocuments()
			So(err, ShouldNotBeNil)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(*mongoImport.SessionProvider, expectedDocuments), ShouldBeNil)
		})
		Convey("an error should be thrown for JSON import on test data that "+
			"is a JSON array without passing --jsonArray", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.File = "testdata/test_array.json"
			mongoImport.IngestOptions.WriteConcern = "1"
			_, err = mongoImport.ImportDocuments()
			So(err, ShouldNotBeNil)
		})
		Convey("an error should be thrown if a plain JSON file is supplied", func() {
			fileHandle, err := os.Open("testdata/test_plain.json")
			So(err, ShouldBeNil)
			jsonInputReader := NewJSONInputReader(true, fileHandle, 1)
			errChan := make(chan error)
			docChan := make(chan bson.D, 1)
			go jsonInputReader.StreamDocument(true, docChan, errChan)
			So(<-errChan, ShouldNotBeNil)
		})
		Convey("an error should be thrown for invalid CSV import on test data", func() {
			mongoImport, err := NewMongoImport()
			So(err, ShouldBeNil)
			mongoImport.InputOptions.Type = CSV
			mongoImport.InputOptions.File = "testdata/test_bad.csv"
			fields := "_id,b,c"
			mongoImport.InputOptions.Fields = &fields
			mongoImport.IngestOptions.StopOnError = true
			mongoImport.IngestOptions.WriteConcern = "1"
			mongoImport.IngestOptions.MaintainInsertionOrder = true
			_, err = mongoImport.ImportDocuments()
			So(err, ShouldNotBeNil)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
			}
			So(checkOnlyHasDocuments(*mongoImport.SessionProvider, expectedDocuments), ShouldBeNil)
		})
	})
}
