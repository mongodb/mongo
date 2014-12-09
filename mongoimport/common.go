package mongoimport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2/bson"
	"gopkg.in/tomb.v2"
	"io"
	"sort"
	"strconv"
	"strings"
)

// ConvertibleDoc is an interface implemented by special types which wrap data
// gotten for various input readers - i.e. CSV, JSON, TSV. It exposes one
// function - Convert() - which converts the special type to a bson.D document
type ConvertibleDoc interface {
	Convert() (bson.D, error)
}

// ImportWorker is used to process documents concurrently
type ImportWorker struct {
	// unprocessedDataChan is used to stream the input data for a worker to process
	unprocessedDataChan chan ConvertibleDoc

	// used to stream the processed document back to the caller
	processedDocumentChan chan bson.D

	// used to synchronise all worker goroutines
	tomb *tomb.Tomb
}

// constructUpsertDocument constructs a BSON document to use for upserts
func constructUpsertDocument(upsertFields []string, document bson.M) bson.M {
	upsertDocument := bson.M{}
	var hasDocumentKey bool
	for _, key := range upsertFields {
		upsertDocument[key] = getUpsertValue(key, document)
		if upsertDocument[key] != nil {
			hasDocumentKey = true
		}
	}
	if !hasDocumentKey {
		return nil
	}
	return upsertDocument
}

// doSequentialStreaming takes a slice of workers, a readDocChan (input) channel and
// an outputChan (output) channel. It sequentially writes unprocessed data read from
// the input channel to each worker and then sequentially reads the processed data
//  from each worker before passing it on to the output channel
func doSequentialStreaming(workers []*ImportWorker, readDocChan chan ConvertibleDoc, outputChan chan bson.D) {
	numWorkers := len(workers)

	// feed in the data to be processed and do round-robin
	// reads from each worker once processing is completed
	go func() {
		i := 0
		for doc := range readDocChan {
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

// getParsedValue returns the appropriate concrete type for the given token
// it first attempts to convert it to an int, if that doesn't succeed, it
// attempts conversion to a float, if that doesn't succeed, it returns the
// token as is.
func getParsedValue(token string) interface{} {
	parsedInt, err := strconv.Atoi(token)
	if err == nil {
		return parsedInt
	}
	parsedFloat, err := strconv.ParseFloat(token, 64)
	if err == nil {
		return parsedFloat
	}
	return token
}

// getUpsertValue takes a given BSON document and a given field, and returns the
// field's associated value in the document. The field is specified using dot
// notation for nested fields. e.g. "person.age" would return 34 would return
// 34 in the document: bson.M{"person": bson.M{"age": 34}} whereas,
// "person.name" would return nil
func getUpsertValue(field string, document bson.M) interface{} {
	index := strings.Index(field, ".")
	if index == -1 {
		return document[field]
	}
	left := field[0:index]
	if document[left] == nil {
		return nil
	}
	subDoc, ok := document[left].(bson.M)
	if !ok {
		return nil
	}
	return getUpsertValue(field[index+1:], subDoc)
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
	if err.Error() == db.ErrNoReachableServers.Error() {
		return err
	}
	if err.Error() == io.EOF.Error() {
		err = db.ErrLostConnection
	}
	log.Logf(log.Always, "error inserting documents: %v", err)
	if stopOnError || err == db.ErrLostConnection {
		return err
	}
	return nil
}

// removeBlankFields removes empty/blank fields in csv and tsv
func removeBlankFields(document bson.D) bson.D {
	for index, pair := range document {
		if _, ok := pair.Value.(string); ok && pair.Value.(string) == "" {
			document = append(document[:index], document[index+1:]...)
		}
	}
	return document
}

// setNestedValue takes a nested field - in the form "a.b.c" -
// its associated value, and a document. It then assigns that
// value to the appropriate nested field within the document
func setNestedValue(key string, value interface{}, document *bson.D) {
	index := strings.Index(key, ".")
	if index == -1 {
		*document = append(*document, bson.DocElem{key, value})
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
		*document = append(*document, bson.DocElem{keyName, subDocument})
	}
}

// streamDocuments concurrently processes data gotten from the inputChan
// channel in parallel and then sends over the processed data to the outputChan
// channel - either in sequence or concurrently (depending on the value of
// ordered) - in which the data was received
func streamDocuments(ordered bool, numDecoders int, readDocChan chan ConvertibleDoc, outputChan chan bson.D) error {
	if numDecoders == 0 {
		numDecoders = 1
	}
	var importWorkers []*ImportWorker

	importTomb := &tomb.Tomb{}

	inChan := readDocChan
	outChan := outputChan
	for i := 0; i < numDecoders; i++ {
		if ordered {
			inChan = make(chan ConvertibleDoc, workerBufferSize)
			outChan = make(chan bson.D, workerBufferSize)
		}
		importWorker := &ImportWorker{
			unprocessedDataChan:   inChan,
			processedDocumentChan: outChan,
			tomb: importTomb,
		}
		importWorkers = append(importWorkers, importWorker)
		importTomb.Go(func() error {
			return importWorker.processDocuments(ordered)
		})
	}

	// if ordered, we have to coordinate the sequence in which processed
	// documents are passed to the main read channel
	if ordered {
		doSequentialStreaming(importWorkers, readDocChan, outputChan)
	}
	err := importTomb.Wait()
	close(outputChan)
	return err
}

// tokensToBSON reads in slice of records - along with ordered fields names -
// and returns a BSON document for the record.
func tokensToBSON(fields, tokens []string, numProcessed uint64) (bson.D, error) {
	log.Logf(log.DebugHigh, "got line: %v", tokens)
	var parsedValue interface{}
	document := bson.D{}
	for index, token := range tokens {
		parsedValue = getParsedValue(token)
		if index < len(fields) {
			if strings.Index(fields[index], ".") != -1 {
				setNestedValue(fields[index], parsedValue, &document)
			} else {
				document = append(document, bson.DocElem{fields[index], parsedValue})
			}
		} else {
			key := "field" + strconv.Itoa(index)
			if util.StringSliceContains(fields, key) {
				return nil, fmt.Errorf("Duplicate field name - on %v - for token #%v ('%v') in document #%v",
					key, index+1, parsedValue, numProcessed)
			}
			document = append(document, bson.DocElem{key, parsedValue})
		}
	}
	return document, nil
}

// validateFields takes a slice of fields and returns an error if the fields
// are incompatible, returns nil otherwise
func validateFields(fields []string) error {
	fieldsCopy := make([]string, len(fields), len(fields))
	copy(fieldsCopy, fields)
	sort.Sort(sort.StringSlice(fieldsCopy))

	for index, field := range fieldsCopy {
		if strings.HasSuffix(field, ".") {
			return fmt.Errorf("field '%v' can not end with a '.'", field)
		}
		if strings.HasPrefix(field, ".") {
			return fmt.Errorf("field '%v' can not start with a '.'", field)
		}
		if strings.HasPrefix(field, "$") {
			return fmt.Errorf("field '%v' can not start with a '$'", field)
		}
		if strings.Contains(field, "..") {
			return fmt.Errorf("field '%v' can not contain consecutive '.' characters", field)
		}
		// NOTE: since fields is sorted, this check ensures that no field
		// is incompatible with another one that occurs further down the list.
		// meant to prevent cases where we have fields like "a" and "a.c"
		for _, latterField := range fieldsCopy[index+1:] {
			// NOTE: this means we will not support imports that have fields that
			// include e.g. a, a.b
			if strings.HasPrefix(latterField, field+".") {
				return fmt.Errorf("incompatible fields found: '%v' and '%v'", field, latterField)
			}
			// NOTE: this means we will not support imports that have fields like
			// a, a - since this is invalid in MongoDB
			if field == latterField {
				return fmt.Errorf("fields can not be identical: '%v' and '%v'", field, latterField)
			}
		}
	}
	return nil
}

// processDocuments reads from the ConvertibleDoc channel and for each record, converts it
// to a bson.D document before sending it on the processedDocumentChan channel. Once the
// input channel is closed the processed channel is also closed if the worker streams its
// reads in order
func (importWorker *ImportWorker) processDocuments(ordered bool) error {
	if ordered {
		defer close(importWorker.processedDocumentChan)
	}
	for {
		select {
		case convertibleDoc, alive := <-importWorker.unprocessedDataChan:
			if !alive {
				return nil
			}
			document, err := convertibleDoc.Convert()
			if err != nil {
				return err
			}
			importWorker.processedDocumentChan <- document
		case <-importWorker.tomb.Dying():
			return nil
		}
	}
}
