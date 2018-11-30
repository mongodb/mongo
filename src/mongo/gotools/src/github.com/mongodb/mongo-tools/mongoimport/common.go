// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoimport

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"

	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2/bson"
	"gopkg.in/tomb.v2"
)

type ParseGrace int

const (
	pgAutoCast ParseGrace = iota
	pgSkipField
	pgSkipRow
	pgStop
)

// ValidatePG ensures the user-provided parseGrace is one of the allowed
// values.
func ValidatePG(pg string) (ParseGrace, error) {
	switch pg {
	case "autoCast":
		return pgAutoCast, nil
	case "skipField":
		return pgSkipField, nil
	case "skipRow":
		return pgSkipRow, nil
	case "stop":
		return pgStop, nil
	default:
		return pgAutoCast, fmt.Errorf("invalid parse grace: %s", pg)
	}
}

// ParsePG interprets the user-provided parseGrace, assuming it is valid.
func ParsePG(pg string) (res ParseGrace) {
	res, _ = ValidatePG(pg)
	return
}

// Converter is an interface that adds the basic Convert method which returns a
// valid BSON document that has been converted by the underlying implementation.
// If conversion fails, err will be set.
type Converter interface {
	Convert() (document bson.D, err error)
}

// An importWorker reads Converter from the unprocessedDataChan channel and
// sends processed BSON documents on the processedDocumentChan channel
type importWorker struct {
	// unprocessedDataChan is used to stream the input data for a worker to process
	unprocessedDataChan chan Converter

	// used to stream the processed document back to the caller
	processedDocumentChan chan bson.D

	// used to synchronise all worker goroutines
	tomb *tomb.Tomb
}

// an interface for tracking the number of bytes, which is used in mongoimport to feed
// the progress bar.
type sizeTracker interface {
	Size() int64
}

// sizeTrackingReader implements Reader and sizeTracker by wrapping an io.Reader and keeping track
// of the total number of bytes read from each call to Read().
type sizeTrackingReader struct {
	bytesRead int64
	reader    io.Reader
}

func (str *sizeTrackingReader) Size() int64 {
	bytes := atomic.LoadInt64(&str.bytesRead)
	return bytes
}

func (str *sizeTrackingReader) Read(p []byte) (n int, err error) {
	n, err = str.reader.Read(p)
	atomic.AddInt64(&str.bytesRead, int64(n))
	return
}

func newSizeTrackingReader(reader io.Reader) *sizeTrackingReader {
	return &sizeTrackingReader{
		reader:    reader,
		bytesRead: 0,
	}
}

var (
	UTF8_BOM = []byte{0xEF, 0xBB, 0xBF}
)

// bomDiscardingReader implements and wraps io.Reader, discarding the UTF-8 BOM, if applicable
type bomDiscardingReader struct {
	buf     *bufio.Reader
	didRead bool
}

func (bd *bomDiscardingReader) Read(p []byte) (int, error) {
	if !bd.didRead {
		bom, err := bd.buf.Peek(3)
		if err == nil && bytes.Equal(bom, UTF8_BOM) {
			bd.buf.Read(make([]byte, 3)) // discard BOM
		}
		bd.didRead = true
	}
	return bd.buf.Read(p)
}

func newBomDiscardingReader(r io.Reader) *bomDiscardingReader {
	return &bomDiscardingReader{buf: bufio.NewReader(r)}
}

// channelQuorumError takes a channel and a quorum - which specifies how many
// messages to receive on that channel before returning. It either returns the
// first non-nil error received on the channel or nil if up to `quorum` nil
// errors are received
func channelQuorumError(ch <-chan error, quorum int) (err error) {
	for i := 0; i < quorum; i++ {
		if err = <-ch; err != nil {
			return
		}
	}
	return
}

// constructUpsertDocument constructs a BSON document to use for upserts
func constructUpsertDocument(upsertFields []string, document bson.D) bson.D {
	upsertDocument := bson.D{}
	var hasDocumentKey bool
	for _, key := range upsertFields {
		val := getUpsertValue(key, document)
		if val != nil {
			hasDocumentKey = true
		}
		upsertDocument = append(upsertDocument, bson.DocElem{Name: key, Value: val})
	}
	if !hasDocumentKey {
		return nil
	}
	return upsertDocument
}

// doSequentialStreaming takes a slice of workers, a readDocs (input) channel and
// an outputChan (output) channel. It sequentially writes unprocessed data read from
// the input channel to each worker and then sequentially reads the processed data
// from each worker before passing it on to the output channel
func doSequentialStreaming(workers []*importWorker, readDocs chan Converter, outputChan chan bson.D) {
	numWorkers := len(workers)

	// feed in the data to be processed and do round-robin
	// reads from each worker once processing is completed
	go func() {
		i := 0
		for doc := range readDocs {
			workers[i].unprocessedDataChan <- doc
			i = (i + 1) % numWorkers
		}

		// close the read channels of all the workers
		for i := 0; i < numWorkers; i++ {
			close(workers[i].unprocessedDataChan)
		}
	}()

	// coordinate the order in which the documents are sent over to the
	// main output channel
	numDoneWorkers := 0
	i := 0
	for {
		processedDocument, open := <-workers[i].processedDocumentChan
		if open {
			outputChan <- processedDocument
		} else {
			numDoneWorkers++
		}
		if numDoneWorkers == numWorkers {
			break
		}
		i = (i + 1) % numWorkers
	}
}

// getUpsertValue takes a given BSON document and a given field, and returns the
// field's associated value in the document. The field is specified using dot
// notation for nested fields. e.g. "person.age" would return 34 would return
// 34 in the document: bson.M{"person": bson.M{"age": 34}} whereas,
// "person.name" would return nil
func getUpsertValue(field string, document bson.D) interface{} {
	index := strings.Index(field, ".")
	if index == -1 {
		// grab the value (ignoring errors because we are okay with nil)
		val, _ := bsonutil.FindValueByKey(field, &document)
		return val
	}
	// recurse into subdocuments
	left := field[0:index]
	subDoc, _ := bsonutil.FindValueByKey(left, &document)
	if subDoc == nil {
		return nil
	}
	subDocD, ok := subDoc.(bson.D)
	if !ok {
		return nil
	}
	return getUpsertValue(field[index+1:], subDocD)
}

// filterIngestError accepts a boolean indicating if a non-nil error should be,
// returned as an actual error.
//
// If the error indicates an unreachable server, it returns that immediately.
//
// If the error indicates an invalid write concern was passed, it returns nil
//
// If the error is not nil, it logs the error. If the error is an io.EOF error -
// indicating a lost connection to the server, it sets the error as such.
//
func filterIngestError(stopOnError bool, err error) error {
	if err == nil {
		return nil
	}
	if err.Error() == io.EOF.Error() {
		return fmt.Errorf(db.ErrLostConnection)
	}
	if stopOnError || db.IsConnectionError(err) {
		return err
	}
	log.Logvf(log.Always, "error inserting documents: %v", err)
	return nil
}

// removeBlankFields takes document and returns a new copy in which
// fields with empty/blank values are removed
func removeBlankFields(document bson.D) (newDocument bson.D) {
	for _, keyVal := range document {
		if val, ok := keyVal.Value.(*bson.D); ok {
			keyVal.Value = removeBlankFields(*val)
		}
		if val, ok := keyVal.Value.(string); ok && val == "" {
			continue
		}
		if val, ok := keyVal.Value.(bson.D); ok && val == nil {
			continue
		}
		newDocument = append(newDocument, keyVal)
	}
	return newDocument
}

// setNestedValue takes a nested field - in the form "a.b.c" -
// its associated value, and a document. It then assigns that
// value to the appropriate nested field within the document
func setNestedValue(key string, value interface{}, document *bson.D) {
	index := strings.Index(key, ".")
	if index == -1 {
		*document = append(*document, bson.DocElem{Name: key, Value: value})
		return
	}
	keyName := key[0:index]
	subDocument := &bson.D{}
	elem, err := bsonutil.FindValueByKey(keyName, document)
	if err != nil { // no such key in the document
		elem = nil
	}
	var existingKey bool
	if elem != nil {
		subDocument = elem.(*bson.D)
		existingKey = true
	}
	setNestedValue(key[index+1:], value, subDocument)
	if !existingKey {
		*document = append(*document, bson.DocElem{Name: keyName, Value: subDocument})
	}
}

// streamDocuments concurrently processes data gotten from the inputChan
// channel in parallel and then sends over the processed data to the outputChan
// channel - either in sequence or concurrently (depending on the value of
// ordered) - in which the data was received
func streamDocuments(ordered bool, numDecoders int, readDocs chan Converter, outputChan chan bson.D) (retErr error) {
	if numDecoders == 0 {
		numDecoders = 1
	}
	var importWorkers []*importWorker
	wg := new(sync.WaitGroup)
	importTomb := new(tomb.Tomb)
	inChan := readDocs
	outChan := outputChan
	for i := 0; i < numDecoders; i++ {
		if ordered {
			inChan = make(chan Converter, workerBufferSize)
			outChan = make(chan bson.D, workerBufferSize)
		}
		iw := &importWorker{
			unprocessedDataChan:   inChan,
			processedDocumentChan: outChan,
			tomb: importTomb,
		}
		importWorkers = append(importWorkers, iw)
		wg.Add(1)
		go func(iw importWorker) {
			defer wg.Done()
			// only set the first worker error and cause sibling goroutines
			// to terminate immediately
			err := iw.processDocuments(ordered)
			if err != nil && retErr == nil {
				retErr = err
				iw.tomb.Kill(err)
			}
		}(*iw)
	}

	// if ordered, we have to coordinate the sequence in which processed
	// documents are passed to the main read channel
	if ordered {
		doSequentialStreaming(importWorkers, readDocs, outputChan)
	}
	wg.Wait()
	close(outputChan)
	return
}

// coercionError should only be used as a specific error type to check
// whether tokensToBSON wants the row to print
type coercionError struct{}

func (coercionError) Error() string { return "coercionError" }

// tokensToBSON reads in slice of records - along with ordered column names -
// and returns a BSON document for the record.
func tokensToBSON(colSpecs []ColumnSpec, tokens []string, numProcessed uint64, ignoreBlanks bool) (bson.D, error) {
	log.Logvf(log.DebugHigh, "got line: %v", tokens)
	var parsedValue interface{}
	document := bson.D{}
	for index, token := range tokens {
		if token == "" && ignoreBlanks {
			continue
		}
		if index < len(colSpecs) {
			parsedValue, err := colSpecs[index].Parser.Parse(token)
			if err != nil {
				log.Logvf(log.DebugHigh, "parse failure in document #%d for column '%s',"+
					"could not parse token '%s' to type %s",
					numProcessed, colSpecs[index].Name, token, colSpecs[index].TypeName)
				switch colSpecs[index].ParseGrace {
				case pgAutoCast:
					parsedValue = autoParse(token)
				case pgSkipField:
					continue
				case pgSkipRow:
					log.Logvf(log.Always, "skipping row #%d: %v", numProcessed, tokens)
					return nil, coercionError{}
				case pgStop:
					return nil, fmt.Errorf("type coercion failure in document #%d for column '%s', "+
						"could not parse token '%s' to type %s",
						numProcessed, colSpecs[index].Name, token, colSpecs[index].TypeName)
				}
			}
			if strings.Index(colSpecs[index].Name, ".") != -1 {
				setNestedValue(colSpecs[index].Name, parsedValue, &document)
			} else {
				document = append(document, bson.DocElem{Name: colSpecs[index].Name, Value: parsedValue})
			}
		} else {
			parsedValue = autoParse(token)
			key := "field" + strconv.Itoa(index)
			if util.StringSliceContains(ColumnNames(colSpecs), key) {
				return nil, fmt.Errorf("duplicate field name - on %v - for token #%v ('%v') in document #%v",
					key, index+1, parsedValue, numProcessed)
			}
			document = append(document, bson.DocElem{Name: key, Value: parsedValue})
		}
	}
	return document, nil
}

// validateFields takes a slice of fields and returns an error if the fields
// are invalid, returns nil otherwise
func validateFields(fields []string) error {
	fieldsCopy := make([]string, len(fields), len(fields))
	copy(fieldsCopy, fields)
	sort.Sort(sort.StringSlice(fieldsCopy))

	for index, field := range fieldsCopy {
		if strings.HasSuffix(field, ".") {
			return fmt.Errorf("field '%v' cannot end with a '.'", field)
		}
		if strings.HasPrefix(field, ".") {
			return fmt.Errorf("field '%v' cannot start with a '.'", field)
		}
		if strings.HasPrefix(field, "$") {
			return fmt.Errorf("field '%v' cannot start with a '$'", field)
		}
		if strings.Contains(field, "..") {
			return fmt.Errorf("field '%v' cannot contain consecutive '.' characters", field)
		}
		// NOTE: since fields is sorted, this check ensures that no field
		// is incompatible with another one that occurs further down the list.
		// meant to prevent cases where we have fields like "a" and "a.c"
		for _, latterField := range fieldsCopy[index+1:] {
			// NOTE: this means we will not support imports that have fields that
			// include e.g. a, a.b
			if strings.HasPrefix(latterField, field+".") {
				return fmt.Errorf("fields '%v' and '%v' are incompatible", field, latterField)
			}
			// NOTE: this means we will not support imports that have fields like
			// a, a - since this is invalid in MongoDB
			if field == latterField {
				return fmt.Errorf("fields cannot be identical: '%v' and '%v'", field, latterField)
			}
		}
	}
	return nil
}

// validateReaderFields is a helper to validate fields for input readers
func validateReaderFields(fields []string) error {
	if err := validateFields(fields); err != nil {
		return err
	}
	if len(fields) == 1 {
		log.Logvf(log.Info, "using field: %v", fields[0])
	} else {
		log.Logvf(log.Info, "using fields: %v", strings.Join(fields, ","))
	}
	return nil
}

// processDocuments reads from the Converter channel and for each record, converts it
// to a bson.D document before sending it on the processedDocumentChan channel. Once the
// input channel is closed the processed channel is also closed if the worker streams its
// reads in order
func (iw *importWorker) processDocuments(ordered bool) error {
	if ordered {
		defer close(iw.processedDocumentChan)
	}
	for {
		select {
		case converter, alive := <-iw.unprocessedDataChan:
			if !alive {
				return nil
			}
			document, err := converter.Convert()
			if err != nil {
				return err
			}
			if document == nil {
				continue
			}
			iw.processedDocumentChan <- document
		case <-iw.tomb.Dying():
			return nil
		}
	}
}
