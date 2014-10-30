package mongoimport

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"io"
	"io/ioutil"
	"os"
	"strings"
	"testing"
)

func init() {
	log.SetVerbosity(&options.Verbosity{
		Verbose: []bool{true, true, true, true},
	})
}

var (
	numProcessed       = uint64(0)
	csvConvertibleDocs = []CSVConvertibleDoc{
		CSVConvertibleDoc{
			fields:       []string{"field1", "field2", "field3"},
			data:         []string{"a", "b", "c"},
			numProcessed: &numProcessed,
		},
		CSVConvertibleDoc{
			fields:       []string{"field4", "field5", "field6"},
			data:         []string{"d", "e", "f"},
			numProcessed: &numProcessed,
		},
		CSVConvertibleDoc{
			fields:       []string{"field7", "field8", "field9"},
			data:         []string{"d", "e", "f"},
			numProcessed: &numProcessed,
		},
		CSVConvertibleDoc{
			fields:       []string{"field10", "field11", "field12"},
			data:         []string{"d", "e", "f"},
			numProcessed: &numProcessed,
		},
		CSVConvertibleDoc{
			fields:       []string{"field13", "field14", "field15"},
			data:         []string{"d", "e", "f"},
			numProcessed: &numProcessed,
		},
	}
	expectedDocuments = []bson.D{
		bson.D{
			bson.DocElem{"field1", "a"},
			bson.DocElem{"field2", "b"},
			bson.DocElem{"field3", "c"},
		},
		bson.D{
			bson.DocElem{"field4", "d"},
			bson.DocElem{"field5", "e"},
			bson.DocElem{"field6", "f"},
		},
		bson.D{
			bson.DocElem{"field7", "d"},
			bson.DocElem{"field8", "e"},
			bson.DocElem{"field9", "f"},
		},
		bson.D{
			bson.DocElem{"field10", "d"},
			bson.DocElem{"field11", "e"},
			bson.DocElem{"field12", "f"},
		},
		bson.D{
			bson.DocElem{"field13", "d"},
			bson.DocElem{"field14", "e"},
			bson.DocElem{"field15", "f"},
		},
	}
)

func convertBSONDToRaw(documents []bson.D) []bson.Raw {
	rawBSONDocuments := []bson.Raw{}
	for _, document := range documents {
		rawBytes, err := bson.Marshal(document)
		So(err, ShouldBeNil)
		rawBSONDocuments = append(rawBSONDocuments, bson.Raw{3, rawBytes})
	}
	return rawBSONDocuments
}

func TestValidateHeaders(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given an import input, in validating the headers", t, func() {
		fields := []string{"a", "b", "c"}
		contents := "headerLine1, headerLine2"
		csvFile, err := ioutil.TempFile("", "mongoimport_")
		So(err, ShouldBeNil)
		_, err = io.WriteString(csvFile, contents)
		So(err, ShouldBeNil)
		fileHandle, err := os.Open(csvFile.Name())
		So(err, ShouldBeNil)

		Convey("if headerLine is true, the first line in the input should be used", func() {
			headers, err := validateHeaders(NewCSVInputReader(fields, fileHandle), true)
			So(err, ShouldBeNil)
			So(len(headers), ShouldEqual, 2)
			// spaces are trimed in the header
			So(headers, ShouldResemble, strings.Split(strings.Replace(contents, " ", "", -1), ","))
		})
		Convey("if headerLine is false, the fields passed in should be used", func() {
			headers, err := validateHeaders(NewCSVInputReader(fields, fileHandle), false)
			So(err, ShouldBeNil)
			So(len(headers), ShouldEqual, 3)
			// spaces are trimed in the header
			So(headers, ShouldResemble, fields)
		})
		Convey("if the fields contain '..', an error should be thrown", func() {
			_, err := validateHeaders(NewCSVInputReader([]string{"a..a"}, fileHandle), false)
			So(err, ShouldNotBeNil)
		})
		Convey("if the fields start/end in a '.', an error should be thrown", func() {
			_, err := validateHeaders(NewCSVInputReader([]string{".a"}, fileHandle), false)
			So(err, ShouldNotBeNil)
			_, err = validateHeaders(NewCSVInputReader([]string{"a."}, fileHandle), false)
			So(err, ShouldNotBeNil)
		})
		Convey("if the fields collide, an error should be thrown", func() {
			_, err := validateHeaders(NewCSVInputReader([]string{"a", "a.a"}, fileHandle), false)
			So(err, ShouldNotBeNil)
			_, err = validateHeaders(NewCSVInputReader([]string{"a", "a.ba", "b.a"}, fileHandle), false)
			So(err, ShouldNotBeNil)
			_, err = validateHeaders(NewCSVInputReader([]string{"a", "a.b.c"}, fileHandle), false)
			So(err, ShouldNotBeNil)
		})
		Convey("if the fields don't collide, no error should be thrown", func() {
			_, err := validateHeaders(NewCSVInputReader([]string{"a", "aa"}, fileHandle), false)
			So(err, ShouldBeNil)
			_, err = validateHeaders(NewCSVInputReader([]string{"a", "aa", "b.a", "b.c"}, fileHandle), false)
			So(err, ShouldBeNil)
			_, err = validateHeaders(NewCSVInputReader([]string{"a", "ba", "ab", "b.a"}, fileHandle), false)
			So(err, ShouldBeNil)
			_, err = validateHeaders(NewCSVInputReader([]string{"a", "ba", "ab", "b.a", "b.c.d"}, fileHandle), false)
			So(err, ShouldBeNil)
			_, err = validateHeaders(NewCSVInputReader([]string{"a", "ab.c"}, fileHandle), false)
			So(err, ShouldBeNil)
		})
		Convey("if the fields contain the same keys, an error should be thrown", func() {
			_, err := validateHeaders(NewCSVInputReader([]string{"a", "ba", "a"}, fileHandle), false)
			So(err, ShouldNotBeNil)
		})
	})
}

func TestGetUpsertValue(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given a field and a BSON document, on calling getUpsertValue", t,
		func() {
			Convey("the value of the key should be correct for unnested "+
				"documents", func() {
				bsonDocument := bson.M{"a": 3}
				So(getUpsertValue("a", bsonDocument), ShouldEqual, 3)
			})
			Convey("the value of the key should be correct for nested "+
				"document fields", func() {
				bsonDocument := bson.M{"a": bson.M{"b": 4}}
				So(getUpsertValue("a.b", bsonDocument), ShouldEqual, 4)
			})
			Convey("the value of the key should be nil for unnested document "+
				"fields that do not exist", func() {
				bsonDocument := bson.M{"a": 4}
				So(getUpsertValue("c", bsonDocument), ShouldBeNil)
			})
			Convey("the value of the key should be nil for nested document "+
				"fields that do not exist", func() {
				bsonDocument := bson.M{"a": bson.M{"b": 4}}
				So(getUpsertValue("a.c", bsonDocument), ShouldBeNil)
			})
			Convey("the value of the key should be nil for nil document"+
				"values", func() {
				So(getUpsertValue("a", bson.M{"a": nil}), ShouldBeNil)
			})
		})
}

func TestConstructUpsertDocument(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

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

func TestGetParsedValue(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given a string token to parse", t, func() {
		Convey("an int token should return the underlying int value", func() {
			So(getParsedValue("3"), ShouldEqual, 3)
		})
		Convey("a float token should return the underlying float value", func() {
			So(getParsedValue(".33"), ShouldEqual, 0.33)
		})
		Convey("a string token should return the underlying string value", func() {
			So(getParsedValue("sd"), ShouldEqual, "sd")
		})
	})
}

func TestSetNestedValue(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given a field, its value, and an existing BSON document...", t, func() {
		currentDocument := bson.D{
			bson.DocElem{"a", 3},
			bson.DocElem{"b", &bson.D{bson.DocElem{"c", "d"}}},
		}
		Convey("ensure top level fields are set and others, unchanged", func() {
			testDocument := &currentDocument
			expectedDocument := bson.DocElem{"c", 4}
			setNestedValue("c", 4, testDocument)
			newDocument := *testDocument
			So(len(newDocument), ShouldEqual, 3)
			So(newDocument[2], ShouldResemble, expectedDocument)
		})
		Convey("ensure new nested top-level fields are set and others, unchanged", func() {
			testDocument := &currentDocument
			expectedDocument := bson.D{bson.DocElem{"b", "4"}}
			setNestedValue("c.b", "4", testDocument)
			newDocument := *testDocument
			So(len(newDocument), ShouldEqual, 3)
			So(newDocument[2].Name, ShouldResemble, "c")
			So(*newDocument[2].Value.(*bson.D), ShouldResemble, expectedDocument)
		})
		Convey("ensure existing nested level fields are set and others, unchanged", func() {
			testDocument := &currentDocument
			expectedDocument := bson.D{bson.DocElem{"c", "d"}, bson.DocElem{"d", 9}}
			setNestedValue("b.d", 9, testDocument)
			newDocument := *testDocument
			So(len(newDocument), ShouldEqual, 2)
			So(newDocument[1].Name, ShouldResemble, "b")
			So(*newDocument[1].Value.(*bson.D), ShouldResemble, expectedDocument)
		})
		Convey("ensure subsequent calls update fields accordingly", func() {
			testDocument := &currentDocument
			expectedDocumentOne := bson.D{bson.DocElem{"c", "d"}, bson.DocElem{"d", 9}}
			expectedDocumentTwo := bson.DocElem{"f", 23}
			setNestedValue("b.d", 9, testDocument)
			newDocument := *testDocument
			So(len(newDocument), ShouldEqual, 2)
			So(newDocument[1].Name, ShouldResemble, "b")
			So(*newDocument[1].Value.(*bson.D), ShouldResemble, expectedDocumentOne)
			setNestedValue("f", 23, testDocument)
			newDocument = *testDocument
			So(len(newDocument), ShouldEqual, 3)
			So(newDocument[2], ShouldResemble, expectedDocumentTwo)
		})
	})
}

func TestRemoveBlankFields(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given an unordered BSON document", t, func() {
		Convey("the same document should be returned if there are no blanks",
			func() {
				bsonDocument := bson.D{bson.DocElem{"a", 3}, bson.DocElem{"b", "hello"}}
				newDocument := removeBlankFields(bsonDocument)
				So(bsonDocument, ShouldResemble, newDocument)
			})
		Convey("a new document without blanks should be returned if there are "+
			" blanks", func() {
			bsonDocument := bson.D{bson.DocElem{"a", 3}, bson.DocElem{"b", ""}}
			newDocument := removeBlankFields(bsonDocument)
			expectedDocument := bson.D{bson.DocElem{"a", 3}}
			So(newDocument, ShouldResemble, expectedDocument)
		})
	})
}

func TestTokensToBSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given an slice of fields and tokens to convert to BSON", t, func() {
		Convey("the expected ordered BSON should be produced for the fields/tokens given", func() {
			fields := []string{"a", "b", "c"}
			tokens := []string{"1", "2", "hello"}
			numProcessed := uint64(0)
			expectedDocument := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", "hello"},
			}
			bsonD, err := tokensToBSON(fields, tokens, numProcessed)
			So(err, ShouldBeNil)
			So(bsonD, ShouldResemble, expectedDocument)
		})
		Convey("if there are more tokens than fields, additional fields should be prefixed"+
			" with 'fields' and an index indicating the header number", func() {
			fields := []string{"a", "b", "c"}
			tokens := []string{"1", "2", "hello", "mongodb", "user"}
			numProcessed := uint64(0)
			expectedDocument := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", "hello"},
				bson.DocElem{"field3", "mongodb"},
				bson.DocElem{"field4", "user"},
			}
			bsonD, err := tokensToBSON(fields, tokens, numProcessed)
			So(err, ShouldBeNil)
			So(bsonD, ShouldResemble, expectedDocument)
		})
		Convey("an error should be thrown if duplicate headers are found", func() {
			fields := []string{"a", "b", "field3"}
			tokens := []string{"1", "2", "hello", "mongodb", "user"}
			numProcessed := uint64(0)
			_, err := tokensToBSON(fields, tokens, numProcessed)
			So(err, ShouldNotBeNil)
		})
		Convey("fields with nested values should be set appropriately", func() {
			fields := []string{"a", "b", "c.a"}
			tokens := []string{"1", "2", "hello"}
			numProcessed := uint64(0)
			expectedDocument := bson.D{
				bson.DocElem{"a", 1},
				bson.DocElem{"b", 2},
				bson.DocElem{"c", bson.D{
					bson.DocElem{"a", "hello"},
				}},
			}
			bsonD, err := tokensToBSON(fields, tokens, numProcessed)
			So(err, ShouldBeNil)
			So(expectedDocument[0].Name, ShouldResemble, bsonD[0].Name)
			So(expectedDocument[0].Value, ShouldResemble, bsonD[0].Value)
			So(expectedDocument[1].Name, ShouldResemble, bsonD[1].Name)
			So(expectedDocument[1].Value, ShouldResemble, bsonD[1].Value)
			So(expectedDocument[2].Name, ShouldResemble, bsonD[2].Name)
			So(expectedDocument[2].Value, ShouldResemble, *bsonD[2].Value.(*bson.D))
		})
	})
}

func TestProcessDocuments(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given an import worker", t, func() {
		numProcessed := uint64(0)
		csvConvertibleDocs := []CSVConvertibleDoc{
			CSVConvertibleDoc{
				fields:       []string{"field1", "field2", "field3"},
				data:         []string{"a", "b", "c"},
				numProcessed: &numProcessed,
			},
			CSVConvertibleDoc{
				fields:       []string{"field4", "field5", "field6"},
				data:         []string{"d", "e", "f"},
				numProcessed: &numProcessed,
			},
		}
		expectedDocuments := []bson.D{
			bson.D{
				bson.DocElem{"field1", "a"},
				bson.DocElem{"field2", "b"},
				bson.DocElem{"field3", "c"},
			},
			bson.D{
				bson.DocElem{"field4", "d"},
				bson.DocElem{"field5", "e"},
				bson.DocElem{"field6", "f"},
			},
		}
		Convey("processDocuments should execute the expected conversion for documents, "+
			"pass then on the output channel, and close the input channel if ordered is true", func() {
			inputChannel := make(chan ConvertibleDoc, 100)
			outputChannel := make(chan bson.D, 100)
			importWorker := &ImportWorker{
				unprocessedDataChan:   inputChannel,
				processedDocumentChan: outputChannel,
			}
			inputChannel <- csvConvertibleDocs[0]
			inputChannel <- csvConvertibleDocs[1]
			close(inputChannel)
			So(importWorker.processDocuments(true), ShouldBeNil)
			doc1, open := <-outputChannel
			So(doc1, ShouldResemble, expectedDocuments[0])
			So(open, ShouldEqual, true)
			doc2, open := <-outputChannel
			So(doc2, ShouldResemble, expectedDocuments[1])
			So(open, ShouldEqual, true)
			_, open = <-outputChannel
			So(open, ShouldEqual, false)
		})
		Convey("processDocuments should execute the expected conversion for documents, "+
			"pass then on the output channel, and leave the input channel open if ordered is false", func() {
			inputChannel := make(chan ConvertibleDoc, 100)
			outputChannel := make(chan bson.D, 100)
			importWorker := &ImportWorker{
				unprocessedDataChan:   inputChannel,
				processedDocumentChan: outputChannel,
			}
			inputChannel <- csvConvertibleDocs[0]
			inputChannel <- csvConvertibleDocs[1]
			close(inputChannel)
			So(importWorker.processDocuments(false), ShouldBeNil)
			doc1, open := <-outputChannel
			So(doc1, ShouldResemble, expectedDocuments[0])
			So(open, ShouldEqual, true)
			doc2, open := <-outputChannel
			So(doc2, ShouldResemble, expectedDocuments[1])
			So(open, ShouldEqual, true)
			// close will throw a runtime error if outputChannel is already closed
			close(outputChannel)
		})
	})
}

func TestDoSequentialStreaming(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey("Given some import workers, a ConvertibleDocs input channel and an bson.D output channel", t, func() {
		inputChannel := make(chan ConvertibleDoc, 5)
		outputChannel := make(chan bson.D, 5)
		workerInputChannel := []chan ConvertibleDoc{
			make(chan ConvertibleDoc),
			make(chan ConvertibleDoc),
		}
		workerOutputChannel := []chan bson.D{
			make(chan bson.D),
			make(chan bson.D),
		}
		importWorkers := []*ImportWorker{
			&ImportWorker{
				unprocessedDataChan:   workerInputChannel[0],
				processedDocumentChan: workerOutputChannel[0],
			},
			&ImportWorker{
				unprocessedDataChan:   workerInputChannel[1],
				processedDocumentChan: workerOutputChannel[1],
			},
		}
		Convey("documents moving through the input channel should be processed and returned in sequence", func() {
			// start goroutines to do sequential processing
			for _, importWorker := range importWorkers {
				go importWorker.processDocuments(true)
			}
			// feed in a bunch of documents
			for _, inputCSVDocument := range csvConvertibleDocs {
				inputChannel <- inputCSVDocument
			}
			close(inputChannel)
			doSequentialStreaming(importWorkers, inputChannel, outputChannel)
			for _, document := range expectedDocuments {
				So(<-outputChannel, ShouldResemble, document)
			}
		})
	})
}

func TestStreamDocuments(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UNIT_TEST_TYPE)

	Convey(`Given:
			1. a boolean indicating streaming order
			2. an input channel where documents are streamed in
			3. an output channel where processed documents are streamed out
			4. an error channel where any errors encountered are streamed`, t, func() {
		inputChannel := make(chan ConvertibleDoc, 5)
		outputChannel := make(chan bson.D, 5)
		errorChannel := make(chan error, 5)
		Convey("the entire pipeline should complete without error under normal circumstances", func() {
			// stream in some documents
			for _, csvConvertibleDoc := range csvConvertibleDocs {
				inputChannel <- csvConvertibleDoc
			}
			close(inputChannel)
			streamDocuments(true, inputChannel, outputChannel, errorChannel)
			// ensure documents are streamed out and processed in the correct manner
			for _, expectedDocument := range expectedDocuments {
				So(<-outputChannel, ShouldResemble, expectedDocument)
			}
		})
		Convey("the entire pipeline should complete with error if an error is encountered", func() {
			// stream in some documents - create duplicate headers to simulate an error
			numProcessed := uint64(0)
			csvConvertibleDoc := CSVConvertibleDoc{
				fields:       []string{"field1", "field2"},
				data:         []string{"a", "b", "c"},
				numProcessed: &numProcessed,
			}
			inputChannel <- csvConvertibleDoc
			close(inputChannel)
			go streamDocuments(true, inputChannel, outputChannel, errorChannel)
			// ensure that an error is returned on the error channel
			So(<-errorChannel, ShouldNotBeNil)
		})
	})
}

// This test will only pass with a version of mongod that supports write commands
func TestInsertDocuments(t *testing.T) {
	Convey("Given a set of documents to insert", t, func() {
		toolOptions := getBasicToolOptions()
		sessionProvider, err := db.InitSessionProvider(*toolOptions)
		So(err, ShouldBeNil)
		session, err := sessionProvider.GetSession()
		So(err, ShouldBeNil)
		collection := session.DB(testDB).C(testCollection)
		writeConcern := "majority"

		Convey("an error should be returned if there are duplicate _ids", func() {
			documents := []bson.D{
				bson.D{bson.DocElem{"_id", 1}},
				bson.D{bson.DocElem{"a", 3}},
				bson.D{bson.DocElem{"_id", 1}},
				bson.D{bson.DocElem{"a", 4}},
			}
			numInserted, err := insertDocuments(
				convertBSONDToRaw(documents),
				collection,
				false,
				writeConcern,
			)
			So(err, ShouldNotBeNil)
			So(numInserted, ShouldEqual, 3)
		})
		Convey("no error should be returned if the documents are valid", func() {
			documents := []bson.D{
				bson.D{bson.DocElem{"a", 1}},
				bson.D{bson.DocElem{"a", 2}},
				bson.D{bson.DocElem{"a", 3}},
				bson.D{bson.DocElem{"a", 4}},
			}
			numInserted, err := insertDocuments(
				convertBSONDToRaw(documents),
				collection,
				false,
				writeConcern,
			)
			So(err, ShouldBeNil)
			So(numInserted, ShouldEqual, 4)
		})
		Convey("ordered inserts with duplicates should error out", func() {
			documents := []bson.D{
				bson.D{bson.DocElem{"_id", 1}},
				bson.D{bson.DocElem{"a", 2}},
				bson.D{bson.DocElem{"_id", 1}},
				bson.D{bson.DocElem{"a", 4}},
			}
			numInserted, err := insertDocuments(
				convertBSONDToRaw(documents),
				collection,
				true,
				writeConcern,
			)
			So(err, ShouldNotBeNil)
			So(numInserted, ShouldEqual, 2)
		})
		Convey("ordered inserts without duplicates should work without errors", func() {
			documents := []bson.D{
				bson.D{bson.DocElem{"a", 1}},
				bson.D{bson.DocElem{"a", 2}},
				bson.D{bson.DocElem{"a", 3}},
				bson.D{bson.DocElem{"a", 4}},
			}
			numInserted, err := insertDocuments(
				convertBSONDToRaw(documents),
				collection,
				true,
				writeConcern,
			)
			So(err, ShouldBeNil)
			So(numInserted, ShouldEqual, 4)
		})
		Reset(func() {
			session, err := sessionProvider.GetSession()
			if err != nil {
				t.Fatalf("error getting session: %v", err)
			}
			defer session.Close()
			session.DB(testDB).C(testCollection).DropCollection()
		})
	})
}
