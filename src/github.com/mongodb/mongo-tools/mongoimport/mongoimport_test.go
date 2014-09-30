package mongoimport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/mongoimport/options"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"io"
	"io/ioutil"
	"os"
	"reflect"
	"testing"
)

var (
	testDB         = "db"
	testCollection = "c"
	testServer     = "localhost"
	testPort       = "27017"

	ssl = &commonOpts.SSL{
		UseSSL: false,
	}
	namespace = &commonOpts.Namespace{
		DB:         testDB,
		Collection: testCollection,
	}
	connection = &commonOpts.Connection{
		Host: testServer,
		Port: testPort,
	}
	toolOptions = &commonOpts.ToolOptions{
		SSL:        ssl,
		Namespace:  namespace,
		Connection: connection,
		Auth:       &commonOpts.Auth{},
	}
	sessionProvider, _ = db.InitSessionProvider(*toolOptions)
)

func TestMongoImportValidateSettings(t *testing.T) {
	Convey("Given a mongoimport instance for validation, ", t, func() {
		Convey("an error should be thrown if no database is given", func() {
			namespace := &commonOpts.Namespace{}
			toolOptions := &commonOpts.ToolOptions{
				Namespace: namespace,
			}
			inputOptions := &options.InputOptions{
				Type: CSV,
			}
			ingestOptions := &options.IngestOptions{}
			mongoImport := MongoImport{
				ToolOptions:   toolOptions,
				InputOptions:  inputOptions,
				IngestOptions: ingestOptions,
			}
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if an invalid type is given", func() {
			namespace := &commonOpts.Namespace{
				DB:         testDB,
				Collection: testCollection,
			}
			toolOptions := &commonOpts.ToolOptions{
				Namespace: namespace,
			}
			inputOptions := &options.InputOptions{
				Type: "invalid",
			}
			ingestOptions := &options.IngestOptions{}
			mongoImport := MongoImport{
				ToolOptions:   toolOptions,
				InputOptions:  inputOptions,
				IngestOptions: ingestOptions,
			}
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("an error should be thrown if neither --headerline is supplied "+
			"nor --fields/--fieldFile", func() {
			namespace := &commonOpts.Namespace{
				DB:         testDB,
				Collection: testCollection,
			}
			toolOptions := &commonOpts.ToolOptions{
				Namespace: namespace,
			}
			inputOptions := &options.InputOptions{
				Type: CSV,
			}
			ingestOptions := &options.IngestOptions{}
			mongoImport := MongoImport{
				ToolOptions:   toolOptions,
				InputOptions:  inputOptions,
				IngestOptions: ingestOptions,
			}
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("no error should be thrown if --headerline is not supplied "+
			"but --fields is supplied", func() {
			namespace := &commonOpts.Namespace{
				DB:         testDB,
				Collection: testCollection,
			}
			toolOptions := &commonOpts.ToolOptions{
				Namespace: namespace,
			}
			inputOptions := &options.InputOptions{
				Fields: "a,b,c",
				Type:   CSV,
			}
			ingestOptions := &options.IngestOptions{}
			mongoImport := MongoImport{
				ToolOptions:   toolOptions,
				InputOptions:  inputOptions,
				IngestOptions: ingestOptions,
			}
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("no error should be thrown if no input type is supplied",
			func() {
				namespace := &commonOpts.Namespace{
					DB:         testDB,
					Collection: testCollection,
				}
				toolOptions := &commonOpts.ToolOptions{
					Namespace: namespace,
				}
				inputOptions := &options.InputOptions{
					Fields: "a,b,c",
				}
				ingestOptions := &options.IngestOptions{}
				mongoImport := MongoImport{
					ToolOptions:   toolOptions,
					InputOptions:  inputOptions,
					IngestOptions: ingestOptions,
				}
				So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
			})

		Convey("no error should be thrown if --headerline is not supplied "+
			"but --fieldFile is supplied", func() {
			namespace := &commonOpts.Namespace{
				DB:         testDB,
				Collection: testCollection,
			}
			toolOptions := &commonOpts.ToolOptions{
				Namespace: namespace,
			}
			inputOptions := &options.InputOptions{
				FieldFile: "test.csv",
				Type:      CSV,
			}
			ingestOptions := &options.IngestOptions{}
			mongoImport := MongoImport{
				ToolOptions:   toolOptions,
				InputOptions:  inputOptions,
				IngestOptions: ingestOptions,
			}
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
		})

		Convey("an error should be thrown if no collection and no file is "+
			"supplied", func() {
			namespace := &commonOpts.Namespace{
				DB: testDB,
			}
			toolOptions := &commonOpts.ToolOptions{
				Namespace: namespace,
			}
			inputOptions := &options.InputOptions{
				FieldFile: "test.csv",
				Type:      CSV,
			}
			ingestOptions := &options.IngestOptions{}
			mongoImport := MongoImport{
				ToolOptions:   toolOptions,
				InputOptions:  inputOptions,
				IngestOptions: ingestOptions,
			}
			So(mongoImport.ValidateSettings([]string{}), ShouldNotBeNil)
		})

		Convey("no error should be thrown if no collection but a file is "+
			"supplied and the file name should be used as the collection "+
			"name", func() {
			namespace := &commonOpts.Namespace{
				DB: testDB,
			}
			toolOptions := &commonOpts.ToolOptions{
				Namespace: namespace,
			}
			inputOptions := &options.InputOptions{
				File:      "input",
				FieldFile: "test.csv",
				Type:      CSV,
			}
			ingestOptions := &options.IngestOptions{}
			mongoImport := MongoImport{
				ToolOptions:   toolOptions,
				InputOptions:  inputOptions,
				IngestOptions: ingestOptions,
			}
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
			So(mongoImport.ToolOptions.Namespace.Collection, ShouldEqual,
				mongoImport.InputOptions.File)
		})

		Convey("with no collection name and a file name the base name of the "+
			"file (without the extension) should be used as the collection "+
			"name", func() {
			namespace := &commonOpts.Namespace{
				DB: testDB,
			}
			toolOptions := &commonOpts.ToolOptions{
				Namespace: namespace,
			}
			inputOptions := &options.InputOptions{
				File:      "/path/to/input/file/dot/input.txt",
				FieldFile: "test.csv",
				Type:      CSV,
			}
			ingestOptions := &options.IngestOptions{}
			mongoImport := MongoImport{
				ToolOptions:   toolOptions,
				InputOptions:  inputOptions,
				IngestOptions: ingestOptions,
			}
			So(mongoImport.ValidateSettings([]string{}), ShouldBeNil)
			So(mongoImport.ToolOptions.Namespace.Collection, ShouldEqual,
				"input")
		})
	})
}

func TestGetInputReader(t *testing.T) {
	Convey("Given a mongoimport instance, on calling getInputReader", t,
		func() {
			Convey("an error should be thrown if the given file referenced by "+
				"the reader does not exist", func() {
				inputOptions := &options.InputOptions{
					File: "/path/to/input/file/dot/input.txt",
				}
				mongoImport := MongoImport{
					InputOptions: inputOptions,
				}
				_, err := mongoImport.getInputReader()
				So(err, ShouldNotBeNil)
			})

			Convey("no error should be thrown if the file exists", func() {
				inputOptions := &options.InputOptions{
					File: "testdata/test_array.json",
				}
				mongoImport := MongoImport{
					InputOptions: inputOptions,
				}
				_, err := mongoImport.getInputReader()
				So(err, ShouldBeNil)
			})

			Convey("no error should be thrown if stdin is used", func() {
				inputOptions := &options.InputOptions{
					File: "",
				}
				mongoImport := MongoImport{
					InputOptions: inputOptions,
				}
				_, err := mongoImport.getInputReader()
				So(err, ShouldBeNil)
			})
		})
}

func TestRemoveBlankFields(t *testing.T) {
	Convey("Given an unordered BSON document", t, func() {
		Convey("the same document should be returned if there are no blanks",
			func() {
				bsonDocument := bson.M{"a": 3, "b": "hello"}
				newDocument := removeBlankFields(bsonDocument)
				So(bsonDocument, ShouldResemble, newDocument)
			})
		Convey("a new document without blanks should be returned if there are "+
			" blanks", func() {
			bsonDocument := bson.M{"a": 3, "b": ""}
			newDocument := removeBlankFields(bsonDocument)
			expectedDocument := bson.M{"a": 3}
			So(newDocument, ShouldResemble, expectedDocument)
		})

	})
}

func TestGetImportInput(t *testing.T) {
	Convey("Given a io.Reader on calling getImportInput", t, func() {
		Convey("no error should be thrown if neither --fields nor --fieldFile "+
			"is used", func() {
			inputOptions := &options.InputOptions{
				File: "/path/to/input/file/dot/input.txt",
			}
			mongoImport := MongoImport{
				InputOptions: inputOptions,
			}
			_, err := mongoImport.getImportInput(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown if --fields is used", func() {
			inputOptions := &options.InputOptions{
				Fields: "a,b,c",
				File:   "/path/to/input/file/dot/input.txt",
			}
			mongoImport := MongoImport{
				InputOptions: inputOptions,
			}
			_, err := mongoImport.getImportInput(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown if --fieldFile is used and it "+
			"references a valid file", func() {
			inputOptions := &options.InputOptions{
				FieldFile: "testdata/test_array.json",
			}
			mongoImport := MongoImport{
				InputOptions: inputOptions,
			}
			_, err := mongoImport.getImportInput(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("an error should be thrown if --fieldFile is used and it "+
			"references an invalid file", func() {
			inputOptions := &options.InputOptions{
				FieldFile: "/path/to/input/file/dot/input.txt",
			}
			mongoImport := MongoImport{
				InputOptions: inputOptions,
			}
			_, err := mongoImport.getImportInput(&os.File{})
			So(err, ShouldNotBeNil)
		})
		Convey("no error should be thrown for CSV import inputs", func() {
			inputOptions := &options.InputOptions{
				Type: CSV,
			}
			mongoImport := MongoImport{
				InputOptions: inputOptions,
			}
			_, err := mongoImport.getImportInput(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown for TSV import inputs", func() {
			inputOptions := &options.InputOptions{
				Type: TSV,
			}
			mongoImport := MongoImport{
				InputOptions: inputOptions,
			}
			_, err := mongoImport.getImportInput(&os.File{})
			So(err, ShouldBeNil)
		})
		Convey("no error should be thrown for JSON import inputs", func() {
			inputOptions := &options.InputOptions{
				Type: JSON,
			}
			mongoImport := MongoImport{
				InputOptions: inputOptions,
			}
			_, err := mongoImport.getImportInput(&os.File{})
			So(err, ShouldBeNil)
		})
	})
}

func TestGetUpsertValue(t *testing.T) {
	Convey("Given a field and a BSON document, on calling getUpsertValue", t,
		func() {
			Convey("the value of the key should be correct for unnested "+
				"documents", func() {
				bsonDocument := bson.M{"a": 3}
				value := getUpsertValue("a", bsonDocument)
				So(value, ShouldEqual, 3)
			})
			Convey("the value of the key should be correct for nested "+
				"document fields", func() {
				bsonDocument := bson.M{"a": bson.M{"b": 4}}
				value := getUpsertValue("a.b", bsonDocument)
				So(value, ShouldEqual, 4)
			})
			Convey("the value of the key should be nil for unnested document "+
				"fields that do not exist", func() {
				bsonDocument := bson.M{"a": 4}
				So(getUpsertValue(testCollection, bsonDocument), ShouldBeNil)
			})
			Convey("the value of the key should be nil for nested document "+
				"fields that do not exist", func() {
				bsonDocument := bson.M{"a": bson.M{"b": 4}}
				So(getUpsertValue("a.c", bsonDocument), ShouldBeNil)
			})
			Convey("the value of the key should be nil for nil document"+
				"values", func() {
				bsonDocument := bson.M{"a": nil}
				So(getUpsertValue("a", bsonDocument), ShouldBeNil)
			})
		})
}

func TestConstructUpsertDocument(t *testing.T) {
	Convey("Given a set of upsert fields and a BSON document, on calling "+
		"constructUpsertDocument", t, func() {
		Convey("the key/value combination in the upsert document should be "+
			"correct for unnested documents with single fields", func() {
			bsonDocument := bson.M{"a": 3}
			upsertFields := []string{"a"}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldResemble, bsonDocument)
		})
		Convey("the key/value combination in the upsert document should be "+
			"correct for unnested documents with several fields", func() {
			bsonDocument := bson.M{"a": 3, "b": "string value"}
			upsertFields := []string{"a"}
			expectedDocument := bson.M{"a": 3}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldResemble, expectedDocument)
		})
		Convey("the key/value combination in the upsert document should be "+
			"correct for nested documents with several fields", func() {
			bsonDocument := bson.M{"a": bson.M{testCollection: 4}, "b": "string value"}
			upsertFields := []string{"a.c"}
			expectedDocument := bson.M{"a.c": 4}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldResemble, expectedDocument)
		})
		Convey("the upsert document should be nil if the key does not exist "+
			"in the BSON document", func() {
			bsonDocument := bson.M{"a": 3, "b": "string value"}
			upsertFields := []string{testCollection}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldBeNil)
		})
	})
}

func TestImportDocuments(t *testing.T) {
	Convey("Given a mongoimport instance with which to import documents, on "+
		"calling importDocuments", t, func() {
		Convey("no error should be thrown for CSV import on test data and all "+
			"CSV data lines should be imported correctly", func() {
			toolOptions := getBasicToolOptions()
			inputOptions := &options.InputOptions{
				Type:   CSV,
				File:   "testdata/test.csv",
				Fields: "a,b,c",
			}
			ingestOptions := &options.IngestOptions{}
			sessionProvider, err := db.InitSessionProvider(*toolOptions)
			So(err, ShouldBeNil)
			mongoImport := MongoImport{
				ToolOptions:     toolOptions,
				InputOptions:    inputOptions,
				IngestOptions:   ingestOptions,
				SessionProvider: sessionProvider,
			}
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
		})

		Convey("TOOLS-247: no error should be thrown for JSON import on test "+
			"data and all documents should be imported correctly", func() {
			toolOptions := getBasicToolOptions()
			inputOptions := &options.InputOptions{
				File: "testdata/test_plain2.json",
			}
			ingestOptions := &options.IngestOptions{
				IgnoreBlanks: true,
			}
			sessionProvider, err := db.InitSessionProvider(*toolOptions)
			So(err, ShouldBeNil)
			mongoImport := MongoImport{
				ToolOptions:     toolOptions,
				InputOptions:    inputOptions,
				IngestOptions:   ingestOptions,
				SessionProvider: sessionProvider,
			}
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 10)
		})
		Convey("no error should be thrown for CSV import on test data with "+
			"--ignoreBlanks only fields without blanks should be imported",
			func() {
				toolOptions := getBasicToolOptions()
				inputOptions := &options.InputOptions{
					Type:   CSV,
					File:   "testdata/test_blanks.csv",
					Fields: "_id,b,c",
				}
				ingestOptions := &options.IngestOptions{
					IgnoreBlanks: true,
				}
				sessionProvider, err := db.InitSessionProvider(*toolOptions)
				So(err, ShouldBeNil)
				mongoImport := MongoImport{
					ToolOptions:     toolOptions,
					InputOptions:    inputOptions,
					IngestOptions:   ingestOptions,
					SessionProvider: sessionProvider,
				}
				numImported, err := mongoImport.ImportDocuments()
				So(err, ShouldBeNil)
				So(numImported, ShouldEqual, 3)
				expectedDocuments := []bson.M{
					bson.M{"_id": 1, "b": 2},
					bson.M{"_id": 5, "c": "6e"},
					bson.M{"_id": 7, "b": 8, "c": 6},
				}
				So(checkOnlyHasDocuments(expectedDocuments), ShouldBeNil)
			})
		Convey("no error should be thrown for CSV import on test data without "+
			"--ignoreBlanks supplied - fields with blanks should be imported",
			func() {
				toolOptions := getBasicToolOptions()
				inputOptions := &options.InputOptions{
					Type:   CSV,
					File:   "testdata/test_blanks.csv",
					Fields: "_id,b,c",
				}
				ingestOptions := &options.IngestOptions{}
				sessionProvider, err := db.InitSessionProvider(*toolOptions)
				So(err, ShouldBeNil)
				mongoImport := MongoImport{
					ToolOptions:     toolOptions,
					InputOptions:    inputOptions,
					IngestOptions:   ingestOptions,
					SessionProvider: sessionProvider,
				}
				numImported, err := mongoImport.ImportDocuments()
				So(err, ShouldBeNil)
				So(numImported, ShouldEqual, 3)
				expectedDocuments := []bson.M{
					bson.M{"_id": 1, "b": 2, "c": ""},
					bson.M{"_id": 5, "b": "", "c": "6e"},
					bson.M{"_id": 7, "b": 8, "c": 6},
				}
				So(checkOnlyHasDocuments(expectedDocuments), ShouldBeNil)
			})
		Convey("no error should be thrown for CSV import on test data with "+
			"--upsert", func() {
			toolOptions := getBasicToolOptions()
			inputOptions := &options.InputOptions{
				Type:   CSV,
				File:   "testdata/test.csv",
				Fields: "_id,b,c",
			}
			ingestOptions := &options.IngestOptions{
				Upsert: true,
			}
			sessionProvider, err := db.InitSessionProvider(*toolOptions)
			So(err, ShouldBeNil)
			mongoImport := MongoImport{
				ToolOptions:     toolOptions,
				InputOptions:    inputOptions,
				IngestOptions:   ingestOptions,
				SessionProvider: sessionProvider,
			}
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(expectedDocuments), ShouldBeNil)
		})
		Convey("no error should be thrown for CSV import on test data with "+
			"--stopOnError. Only documents before error should be imported",
			func() {
				toolOptions := getBasicToolOptions()
				inputOptions := &options.InputOptions{
					Type:   CSV,
					File:   "testdata/test.csv",
					Fields: "_id,b,c",
				}
				ingestOptions := &options.IngestOptions{
					StopOnError: true,
				}
				sessionProvider, err := db.InitSessionProvider(*toolOptions)
				So(err, ShouldBeNil)
				mongoImport := MongoImport{
					ToolOptions:     toolOptions,
					InputOptions:    inputOptions,
					IngestOptions:   ingestOptions,
					SessionProvider: sessionProvider,
				}
				numImported, err := mongoImport.ImportDocuments()
				So(err, ShouldBeNil)
				So(numImported, ShouldEqual, 3)
				expectedDocuments := []bson.M{
					bson.M{"_id": 1, "b": 2, "c": 3},
					bson.M{"_id": 3, "b": 5.4, "c": "string"},
					bson.M{"_id": 5, "b": 6, "c": 6},
				}
				So(checkOnlyHasDocuments(expectedDocuments), ShouldBeNil)
			})
		Convey("no error should be thrown for CSV import on test data with "+
			"duplicate _id if --stopOnError is not set", func() {
			toolOptions := getBasicToolOptions()
			inputOptions := &options.InputOptions{
				Type:   CSV,
				File:   "testdata/test_duplicate.csv",
				Fields: "_id,b,c",
			}
			ingestOptions := &options.IngestOptions{}
			sessionProvider, err := db.InitSessionProvider(*toolOptions)
			So(err, ShouldBeNil)
			mongoImport := MongoImport{
				ToolOptions:     toolOptions,
				InputOptions:    inputOptions,
				IngestOptions:   ingestOptions,
				SessionProvider: sessionProvider,
			}
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 4)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 6},
				bson.M{"_id": 8, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(expectedDocuments), ShouldBeNil)
		})
		Convey("no error should be thrown for CSV import on test data with "+
			"--drop", func() {
			toolOptions := getBasicToolOptions()
			inputOptions := &options.InputOptions{
				Type:   CSV,
				File:   "testdata/test.csv",
				Fields: "_id,b,c",
			}
			ingestOptions := &options.IngestOptions{
				Drop: true,
			}
			sessionProvider, err := db.InitSessionProvider(*toolOptions)
			So(err, ShouldBeNil)
			mongoImport := MongoImport{
				ToolOptions:     toolOptions,
				InputOptions:    inputOptions,
				IngestOptions:   ingestOptions,
				SessionProvider: sessionProvider,
			}
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(expectedDocuments), ShouldBeNil)
		})
		Convey("no error should be thrown for CSV import on test data with "+
			"--headerLine", func() {
			toolOptions := getBasicToolOptions()
			inputOptions := &options.InputOptions{
				Type:       CSV,
				File:       "testdata/test.csv",
				HeaderLine: true,
			}
			ingestOptions := &options.IngestOptions{}
			sessionProvider, err := db.InitSessionProvider(*toolOptions)
			So(err, ShouldBeNil)
			mongoImport := MongoImport{
				ToolOptions:     toolOptions,
				InputOptions:    inputOptions,
				IngestOptions:   ingestOptions,
				SessionProvider: sessionProvider,
			}
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 2)
		})
		Convey("EOF should be thrown for CSV import on test data with "+
			"--headerLine if the input file is empty", func() {
			toolOptions := getBasicToolOptions()
			csvFile, err := ioutil.TempFile("", "mongoimport_")
			So(err, ShouldBeNil)
			csvFile.Close()
			inputOptions := &options.InputOptions{
				Type:       CSV,
				File:       csvFile.Name(),
				HeaderLine: true,
			}
			ingestOptions := &options.IngestOptions{}
			sessionProvider, err := db.InitSessionProvider(*toolOptions)
			So(err, ShouldBeNil)
			mongoImport := MongoImport{
				ToolOptions:     toolOptions,
				InputOptions:    inputOptions,
				IngestOptions:   ingestOptions,
				SessionProvider: sessionProvider,
			}
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldEqual, io.EOF)
			So(numImported, ShouldEqual, 0)
		})
		Convey("no error should be thrown for CSV import on test data with "+
			"--upsert and --upsertFields", func() {
			toolOptions := getBasicToolOptions()
			inputOptions := &options.InputOptions{
				Type:   CSV,
				File:   "testdata/test.csv",
				Fields: "_id,b,c",
			}
			ingestOptions := &options.IngestOptions{
				Upsert:       true,
				UpsertFields: "_id",
			}
			sessionProvider, err := db.InitSessionProvider(*toolOptions)
			So(err, ShouldBeNil)
			mongoImport := MongoImport{
				ToolOptions:     toolOptions,
				InputOptions:    inputOptions,
				IngestOptions:   ingestOptions,
				SessionProvider: sessionProvider,
			}
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(expectedDocuments), ShouldBeNil)
		})
		Convey("no error should be thrown for CSV import on test data with "+
			"--upsert and --upsertFields and duplicate _id if --stopOnError "+
			"is not set", func() {
			inputOptions := &options.InputOptions{
				Type:   CSV,
				File:   "testdata/test_duplicate.csv",
				Fields: "_id,b,c",
			}
			ingestOptions := &options.IngestOptions{
				Upsert:       true,
				UpsertFields: "_id",
			}
			toolOptions := getBasicToolOptions()
			sessionProvider, err := db.InitSessionProvider(*toolOptions)
			So(err, ShouldBeNil)
			mongoImport := MongoImport{
				ToolOptions:     toolOptions,
				InputOptions:    inputOptions,
				IngestOptions:   ingestOptions,
				SessionProvider: sessionProvider,
			}
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldBeNil)
			So(numImported, ShouldEqual, 5)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 9},
				bson.M{"_id": 8, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(expectedDocuments), ShouldBeNil)
		})
		Convey("an error should be thrown for CSV import on test data with "+
			"duplicate _id if --stopOnError is set", func() {
			toolOptions := getBasicToolOptions()
			inputOptions := &options.InputOptions{
				Type:   CSV,
				File:   "testdata/test_duplicate.csv",
				Fields: "_id,b,c",
			}
			ingestOptions := &options.IngestOptions{
				StopOnError: true,
			}
			sessionProvider, err := db.InitSessionProvider(*toolOptions)
			So(err, ShouldBeNil)
			mongoImport := MongoImport{
				ToolOptions:     toolOptions,
				InputOptions:    inputOptions,
				IngestOptions:   ingestOptions,
				SessionProvider: sessionProvider,
			}
			numImported, err := mongoImport.ImportDocuments()
			So(err, ShouldNotBeNil)
			So(numImported, ShouldEqual, 3)
			expectedDocuments := []bson.M{
				bson.M{"_id": 1, "b": 2, "c": 3},
				bson.M{"_id": 3, "b": 5.4, "c": "string"},
				bson.M{"_id": 5, "b": 6, "c": 6},
			}
			So(checkOnlyHasDocuments(expectedDocuments), ShouldBeNil)
		})
		Convey("an error should be thrown for invalid CSV import on test data",
			func() {
				inputOptions := &options.InputOptions{
					Type:   CSV,
					File:   "testdata/test_bad.csv",
					Fields: "_id,b,c",
				}
				toolOptions := getBasicToolOptions()
				ingestOptions := &options.IngestOptions{}
				sessionProvider, err := db.InitSessionProvider(*toolOptions)
				So(err, ShouldBeNil)
				mongoImport := MongoImport{
					ToolOptions:     toolOptions,
					InputOptions:    inputOptions,
					IngestOptions:   ingestOptions,
					SessionProvider: sessionProvider,
				}
				numImported, err := mongoImport.ImportDocuments()
				So(err, ShouldNotBeNil)
				So(numImported, ShouldEqual, 1)
				expectedDocuments := []bson.M{
					bson.M{"_id": 1, "b": 2, "c": 3},
				}
				So(checkOnlyHasDocuments(expectedDocuments), ShouldBeNil)
			})
		Reset(func() {
			session, err := sessionProvider.GetSession()
			if err != nil {
				t.Fatalf("Error doing cleanup: %v", err)
			}
			defer session.Close()
			session.DB(testDB).C(testCollection).DropCollection()
		})

	})
}

// checkOnlyHasDocuments returns an error if the documents in the test
// collection don't exactly match those that are passed in
func checkOnlyHasDocuments(expectedDocuments []bson.M) error {
	session, err := sessionProvider.GetSession()
	if err != nil {
		return err
	}
	defer session.Close()

	collection := session.DB(testDB).C(testCollection)
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
// for calls to ImportDocument
func getBasicToolOptions() *commonOpts.ToolOptions {
	ssl := &commonOpts.SSL{
		UseSSL: false,
	}
	namespace := &commonOpts.Namespace{
		DB:         testDB,
		Collection: testCollection,
	}
	connection := &commonOpts.Connection{
		Host: testServer,
		Port: testPort,
	}
	return &commonOpts.ToolOptions{
		SSL:        ssl,
		Namespace:  namespace,
		Connection: connection,
		Auth:       &commonOpts.Auth{},
	}
}
