// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoimport

import (
	"fmt"
	"io"
	"testing"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
	"gopkg.in/tomb.v2"
)

func init() {
	log.SetVerbosity(&options.Verbosity{
		VLevel: 4,
	})
}

var (
	index         = uint64(0)
	csvConverters = []CSVConverter{
		CSVConverter{
			colSpecs: []ColumnSpec{
				{"field1", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field2", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field3", new(FieldAutoParser), pgAutoCast, "auto"},
			},
			data:  []string{"a", "b", "c"},
			index: index,
		},
		CSVConverter{
			colSpecs: []ColumnSpec{
				{"field4", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field5", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field6", new(FieldAutoParser), pgAutoCast, "auto"},
			},
			data:  []string{"d", "e", "f"},
			index: index,
		},
		CSVConverter{
			colSpecs: []ColumnSpec{
				{"field7", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field8", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field9", new(FieldAutoParser), pgAutoCast, "auto"},
			},
			data:  []string{"d", "e", "f"},
			index: index,
		},
		CSVConverter{
			colSpecs: []ColumnSpec{
				{"field10", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field11", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field12", new(FieldAutoParser), pgAutoCast, "auto"},
			},
			data:  []string{"d", "e", "f"},
			index: index,
		},
		CSVConverter{
			colSpecs: []ColumnSpec{
				{"field13", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field14", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field15", new(FieldAutoParser), pgAutoCast, "auto"},
			},
			data:  []string{"d", "e", "f"},
			index: index,
		},
	}
	expectedDocuments = []bson.D{
		{
			{"field1", "a"},
			{"field2", "b"},
			{"field3", "c"},
		}, {
			{"field4", "d"},
			{"field5", "e"},
			{"field6", "f"},
		}, {
			{"field7", "d"},
			{"field8", "e"},
			{"field9", "f"},
		}, {
			{"field10", "d"},
			{"field11", "e"},
			{"field12", "f"},
		}, {
			{"field13", "d"},
			{"field14", "e"},
			{"field15", "f"},
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

func TestValidateFields(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Given an import input, in validating the headers", t, func() {
		Convey("if the fields contain '..', an error should be thrown", func() {
			So(validateFields([]string{"a..a"}), ShouldNotBeNil)
		})
		Convey("if the fields start/end in a '.', an error should be thrown", func() {
			So(validateFields([]string{".a"}), ShouldNotBeNil)
			So(validateFields([]string{"a."}), ShouldNotBeNil)
		})
		Convey("if the fields start in a '$', an error should be thrown", func() {
			So(validateFields([]string{"$.a"}), ShouldNotBeNil)
			So(validateFields([]string{"$"}), ShouldNotBeNil)
			So(validateFields([]string{"$a"}), ShouldNotBeNil)
			So(validateFields([]string{"a$a"}), ShouldBeNil)
		})
		Convey("if the fields collide, an error should be thrown", func() {
			So(validateFields([]string{"a", "a.a"}), ShouldNotBeNil)
			So(validateFields([]string{"a", "a.ba", "b.a"}), ShouldNotBeNil)
			So(validateFields([]string{"a", "a.ba", "b.a"}), ShouldNotBeNil)
			So(validateFields([]string{"a", "a.b.c"}), ShouldNotBeNil)
		})
		Convey("if the fields don't collide, no error should be thrown", func() {
			So(validateFields([]string{"a", "aa"}), ShouldBeNil)
			So(validateFields([]string{"a", "aa", "b.a", "b.c"}), ShouldBeNil)
			So(validateFields([]string{"a", "ba", "ab", "b.a"}), ShouldBeNil)
			So(validateFields([]string{"a", "ba", "ab", "b.a", "b.c.d"}), ShouldBeNil)
			So(validateFields([]string{"a", "ab.c"}), ShouldBeNil)
		})
		Convey("if the fields contain the same keys, an error should be thrown", func() {
			So(validateFields([]string{"a", "ba", "a"}), ShouldNotBeNil)
		})
	})
}

func TestGetUpsertValue(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Given a field and a BSON document, on calling getUpsertValue", t, func() {
		Convey("the value of the key should be correct for unnested documents", func() {
			bsonDocument := bson.D{{"a", 3}}
			So(getUpsertValue("a", bsonDocument), ShouldEqual, 3)
		})
		Convey("the value of the key should be correct for nested document fields", func() {
			inner := bson.D{{"b", 4}}
			bsonDocument := bson.D{{"a", inner}}
			So(getUpsertValue("a.b", bsonDocument), ShouldEqual, 4)
		})
		Convey("the value of the key should be nil for unnested document "+
			"fields that do not exist", func() {
			bsonDocument := bson.D{{"a", 4}}
			So(getUpsertValue("c", bsonDocument), ShouldBeNil)
		})
		Convey("the value of the key should be nil for nested document "+
			"fields that do not exist", func() {
			inner := bson.D{{"b", 4}}
			bsonDocument := bson.D{{"a", inner}}
			So(getUpsertValue("a.c", bsonDocument), ShouldBeNil)
		})
		Convey("the value of the key should be nil for nil document values", func() {
			So(getUpsertValue("a", bson.D{{"a", nil}}), ShouldBeNil)
		})
	})
}

func TestConstructUpsertDocument(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Given a set of upsert fields and a BSON document, on calling "+
		"constructUpsertDocument", t, func() {
		Convey("the key/value combination in the upsert document should be "+
			"correct for unnested documents with single fields", func() {
			bsonDocument := bson.D{{"a", 3}}
			upsertFields := []string{"a"}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldResemble, bsonDocument)
		})
		Convey("the key/value combination in the upsert document should be "+
			"correct for unnested documents with several fields", func() {
			bsonDocument := bson.D{{"a", 3}, {"b", "string value"}}
			upsertFields := []string{"a"}
			expectedDocument := bson.D{{"a", 3}}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldResemble, expectedDocument)
		})
		Convey("the key/value combination in the upsert document should be "+
			"correct for nested documents with several fields", func() {
			inner := bson.D{{testCollection, 4}}
			bsonDocument := bson.D{{"a", inner}, {"b", "string value"}}
			upsertFields := []string{"a.c"}
			expectedDocument := bson.D{{"a.c", 4}}
			upsertDocument := constructUpsertDocument(upsertFields,
				bsonDocument)
			So(upsertDocument, ShouldResemble, expectedDocument)
		})
		Convey("the upsert document should be nil if the key does not exist "+
			"in the BSON document", func() {
			bsonDocument := bson.D{{"a", 3}, {"b", "string value"}}
			upsertFields := []string{testCollection}
			upsertDocument := constructUpsertDocument(upsertFields, bsonDocument)
			So(upsertDocument, ShouldBeNil)
		})
	})
}

func TestSetNestedValue(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Given a field, its value, and an existing BSON document...", t, func() {
		b := bson.D{{"c", "d"}}
		currentDocument := bson.D{
			{"a", 3},
			{"b", &b},
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
			expectedDocument := bson.D{{"b", "4"}}
			setNestedValue("c.b", "4", testDocument)
			newDocument := *testDocument
			So(len(newDocument), ShouldEqual, 3)
			So(newDocument[2].Name, ShouldResemble, "c")
			So(*newDocument[2].Value.(*bson.D), ShouldResemble, expectedDocument)
		})
		Convey("ensure existing nested level fields are set and others, unchanged", func() {
			testDocument := &currentDocument
			expectedDocument := bson.D{{"c", "d"}, {"d", 9}}
			setNestedValue("b.d", 9, testDocument)
			newDocument := *testDocument
			So(len(newDocument), ShouldEqual, 2)
			So(newDocument[1].Name, ShouldResemble, "b")
			So(*newDocument[1].Value.(*bson.D), ShouldResemble, expectedDocument)
		})
		Convey("ensure subsequent calls update fields accordingly", func() {
			testDocument := &currentDocument
			expectedDocumentOne := bson.D{{"c", "d"}, {"d", 9}}
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
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Given an unordered BSON document", t, func() {
		Convey("the same document should be returned if there are no blanks", func() {
			bsonDocument := bson.D{{"a", 3}, {"b", "hello"}}
			So(removeBlankFields(bsonDocument), ShouldResemble, bsonDocument)
		})
		Convey("a new document without blanks should be returned if there are "+
			" blanks", func() {
			d := bson.D{
				{"a", ""},
				{"b", ""},
			}
			e := bson.D{
				{"a", ""},
				{"b", 1},
			}
			bsonDocument := bson.D{
				{"a", 0},
				{"b", ""},
				{"c", ""},
				{"d", &d},
				{"e", &e},
			}
			inner := bson.D{
				{"b", 1},
			}
			expectedDocument := bson.D{
				{"a", 0},
				{"e", inner},
			}
			So(removeBlankFields(bsonDocument), ShouldResemble, expectedDocument)
		})
	})
}

func TestTokensToBSON(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Given an slice of column specs and tokens to convert to BSON", t, func() {
		Convey("the expected ordered BSON should be produced for the given"+
			"column specs and tokens", func() {
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			tokens := []string{"1", "2", "hello"}
			expectedDocument := bson.D{
				{"a", int32(1)},
				{"b", int32(2)},
				{"c", "hello"},
			}
			bsonD, err := tokensToBSON(colSpecs, tokens, uint64(0), false)
			So(err, ShouldBeNil)
			So(bsonD, ShouldResemble, expectedDocument)
		})
		Convey("if there are more tokens than fields, additional fields should be prefixed"+
			" with 'fields' and an index indicating the header number", func() {
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			tokens := []string{"1", "2", "hello", "mongodb", "user"}
			expectedDocument := bson.D{
				{"a", int32(1)},
				{"b", int32(2)},
				{"c", "hello"},
				{"field3", "mongodb"},
				{"field4", "user"},
			}
			bsonD, err := tokensToBSON(colSpecs, tokens, uint64(0), false)
			So(err, ShouldBeNil)
			So(bsonD, ShouldResemble, expectedDocument)
		})
		Convey("an error should be thrown if duplicate headers are found", func() {
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"field3", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			tokens := []string{"1", "2", "hello", "mongodb", "user"}
			_, err := tokensToBSON(colSpecs, tokens, uint64(0), false)
			So(err, ShouldNotBeNil)
		})
		Convey("fields with nested values should be set appropriately", func() {
			colSpecs := []ColumnSpec{
				{"a", new(FieldAutoParser), pgAutoCast, "auto"},
				{"b", new(FieldAutoParser), pgAutoCast, "auto"},
				{"c.a", new(FieldAutoParser), pgAutoCast, "auto"},
			}
			tokens := []string{"1", "2", "hello"}
			c := bson.D{
				{"a", "hello"},
			}
			expectedDocument := bson.D{
				{"a", int32(1)},
				{"b", int32(2)},
				{"c", c},
			}
			bsonD, err := tokensToBSON(colSpecs, tokens, uint64(0), false)
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
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Given an import worker", t, func() {
		index := uint64(0)
		csvConverters := []CSVConverter{
			CSVConverter{
				colSpecs: []ColumnSpec{
					{"field1", new(FieldAutoParser), pgAutoCast, "auto"},
					{"field2", new(FieldAutoParser), pgAutoCast, "auto"},
					{"field3", new(FieldAutoParser), pgAutoCast, "auto"},
				},
				data:  []string{"a", "b", "c"},
				index: index,
			},
			CSVConverter{
				colSpecs: []ColumnSpec{
					{"field4", new(FieldAutoParser), pgAutoCast, "auto"},
					{"field5", new(FieldAutoParser), pgAutoCast, "auto"},
					{"field6", new(FieldAutoParser), pgAutoCast, "auto"},
				},
				data:  []string{"d", "e", "f"},
				index: index,
			},
		}
		expectedDocuments := []bson.D{
			{
				{"field1", "a"},
				{"field2", "b"},
				{"field3", "c"},
			}, {
				{"field4", "d"},
				{"field5", "e"},
				{"field6", "f"},
			},
		}
		Convey("processDocuments should execute the expected conversion for documents, "+
			"pass then on the output channel, and close the input channel if ordered is true", func() {
			inputChannel := make(chan Converter, 100)
			outputChannel := make(chan bson.D, 100)
			iw := &importWorker{
				unprocessedDataChan:   inputChannel,
				processedDocumentChan: outputChannel,
				tomb: &tomb.Tomb{},
			}
			inputChannel <- csvConverters[0]
			inputChannel <- csvConverters[1]
			close(inputChannel)
			So(iw.processDocuments(true), ShouldBeNil)
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
			inputChannel := make(chan Converter, 100)
			outputChannel := make(chan bson.D, 100)
			iw := &importWorker{
				unprocessedDataChan:   inputChannel,
				processedDocumentChan: outputChannel,
				tomb: &tomb.Tomb{},
			}
			inputChannel <- csvConverters[0]
			inputChannel <- csvConverters[1]
			close(inputChannel)
			So(iw.processDocuments(false), ShouldBeNil)
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
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Given some import workers, a Converters input channel and an bson.D output channel", t, func() {
		inputChannel := make(chan Converter, 5)
		outputChannel := make(chan bson.D, 5)
		workerInputChannel := []chan Converter{
			make(chan Converter),
			make(chan Converter),
		}
		workerOutputChannel := []chan bson.D{
			make(chan bson.D),
			make(chan bson.D),
		}
		importWorkers := []*importWorker{
			&importWorker{
				unprocessedDataChan:   workerInputChannel[0],
				processedDocumentChan: workerOutputChannel[0],
				tomb: &tomb.Tomb{},
			},
			&importWorker{
				unprocessedDataChan:   workerInputChannel[1],
				processedDocumentChan: workerOutputChannel[1],
				tomb: &tomb.Tomb{},
			},
		}
		Convey("documents moving through the input channel should be processed and returned in sequence", func() {
			// start goroutines to do sequential processing
			for _, iw := range importWorkers {
				go iw.processDocuments(true)
			}
			// feed in a bunch of documents
			for _, inputCSVDocument := range csvConverters {
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
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey(`Given:
			1. a boolean indicating streaming order
			2. an input channel where documents are streamed in
			3. an output channel where processed documents are streamed out`, t, func() {

		inputChannel := make(chan Converter, 5)
		outputChannel := make(chan bson.D, 5)

		Convey("the entire pipeline should complete without error under normal circumstances", func() {
			// stream in some documents
			for _, csvConverter := range csvConverters {
				inputChannel <- csvConverter
			}
			close(inputChannel)
			So(streamDocuments(true, 3, inputChannel, outputChannel), ShouldBeNil)

			// ensure documents are streamed out and processed in the correct manner
			for _, expectedDocument := range expectedDocuments {
				So(<-outputChannel, ShouldResemble, expectedDocument)
			}
		})
		Convey("the entire pipeline should complete with error if an error is encountered", func() {
			// stream in some documents - create duplicate headers to simulate an error
			csvConverter := CSVConverter{
				colSpecs: []ColumnSpec{
					{"field1", new(FieldAutoParser), pgAutoCast, "auto"},
					{"field2", new(FieldAutoParser), pgAutoCast, "auto"},
				},
				data:  []string{"a", "b", "c"},
				index: uint64(0),
			}
			inputChannel <- csvConverter
			close(inputChannel)

			// ensure that an error is returned on the error channel
			So(streamDocuments(true, 3, inputChannel, outputChannel), ShouldNotBeNil)
		})
	})
}

func TestChannelQuorumError(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)
	Convey("Given a channel and a quorum...", t, func() {
		Convey("an error should be returned if one is received", func() {
			ch := make(chan error, 2)
			ch <- nil
			ch <- io.EOF
			So(channelQuorumError(ch, 2), ShouldNotBeNil)
		})
		Convey("no error should be returned if none is received", func() {
			ch := make(chan error, 2)
			ch <- nil
			ch <- nil
			So(channelQuorumError(ch, 2), ShouldBeNil)
		})
		Convey("no error should be returned if up to quorum nil errors are received", func() {
			ch := make(chan error, 3)
			ch <- nil
			ch <- nil
			ch <- io.EOF
			So(channelQuorumError(ch, 2), ShouldBeNil)
		})
	})
}

func TestFilterIngestError(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("Given a boolean 'stopOnError' and an error...", t, func() {

		Convey("an error should be returned if stopOnError is true the err is not nil", func() {
			So(filterIngestError(true, fmt.Errorf("")), ShouldNotBeNil)
		})

		Convey("errLostConnection should be returned if stopOnError is true the err is io.EOF", func() {
			So(filterIngestError(true, io.EOF).Error(), ShouldEqual, db.ErrLostConnection)
		})

		Convey("no error should be returned if stopOnError is false the err is not nil", func() {
			So(filterIngestError(false, fmt.Errorf("")), ShouldBeNil)
		})

		Convey("no error should be returned if stopOnError is false the err is nil", func() {
			So(filterIngestError(false, nil), ShouldBeNil)
		})

		Convey("no error should be returned if stopOnError is true the err is nil", func() {
			So(filterIngestError(true, nil), ShouldBeNil)
		})
	})
}
