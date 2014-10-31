package mongoimport

import (
	"errors"
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
	"sort"
	"strconv"
	"strings"
	"sync"
)

var (
	errLostConnection    = errors.New("lost connection to server")
	errNoReachableServer = errors.New("no reachable servers")
	errNsNotFound        = errors.New("ns not found")
	errNoReplSet         = errors.New("norepl")
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

// doSequentialStreaming takes a slice of workers, an input channel and an output
// channel. It sequentially writes unprocessed data read from the input channel
// to each worker and then sequentially reads the processed data from each worker
// before passing it on to the output channel
func doSequentialStreaming(workers []*ImportWorker, input chan ConvertibleDoc, output chan bson.D) {
	numWorkers := len(workers)

	// feed in the data to be processed and do round-robin
	// reads from each worker once processing is completed
	go func() {
		i := 0
		for data := range input {
			workers[i].unprocessedDataChan <- data
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
			output <- processedDocument
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

// handleErr accepts a boolean indicating if a non-nil error should be returned,
// and an actual error. If the error is not nil, it logs. If the error is an
// io.EOF - indicating a lost connection to the server, it sets the error as
// such. In any case, it unconditionally returns an error which may/may not be nil
func handleErr(stopOnError bool, err error) error {
	if err == nil {
		return nil
	}
	if err.Error() == errNoReachableServer.Error() {
		return err
	}
	if err.Error() == errNoReplSet.Error() {
		log.Logf(log.Info, "write concern 'majority' run against standalone")
		return nil
	}
	if err == io.EOF {
		err = errLostConnection
	}
	log.Logf(log.Always, "error inserting documents: %v", err)
	if stopOnError || err == errLostConnection {
		return err
	}
	return nil
}

// insertDocuments writes the given documents to the specified collection and
// returns the number of documents inserted and an error. It can handle both
// ordered and unordered writes. If both a write error and a write concern error
// are encountered, the write error is returned. If the target server is not
// capable of handling write commands, it returns an error.
func insertDocuments(documents []bson.Raw, collection *mgo.Collection, ordered bool, writeConcern string) (int, error) {
	// mongod v2.6 requires you to explicitly pass an integer for numeric write
	// concerns
	var wc interface{}
	if intWriteConcern, err := strconv.Atoi(writeConcern); err != nil {
		wc = writeConcern
	} else {
		wc = intWriteConcern
	}

	response := &db.WriteCommandResponse{}
	err := collection.Database.Run(
		bson.D{
			bson.DocElem{"insert", collection.Name},
			bson.DocElem{"ordered", ordered},
			bson.DocElem{"documents", documents},
			bson.DocElem{"writeConcern", bson.D{bson.DocElem{"w", wc}}},
		}, response)
	if err != nil {
		return 0, err
	}
	// if the write concern is 0, n is not present in the response document
	// so we return immediately with no errors
	if response.NumAffected == nil {
		return len(documents), nil
	}
	if response.Ok == 0 {
		// the command itself failed (authentication failed.., syntax error)
		return 0, fmt.Errorf("write command failed")
	} else if len(response.WriteErrors) != 0 {
		// happens if the server couldn't write the data; e.g. because of a
		// duplicate key, running out of disk space, etc
		for _, writeError := range response.WriteErrors {
			log.Logf(log.Always, writeError.Errmsg)
		}
		return *response.NumAffected, fmt.Errorf("encountered write errors")
	} else if response.WriteConcernError.Errmsg != "" {
		log.Logf(log.Always, response.WriteConcernError.Errmsg)
		return 0, fmt.Errorf("encountered write concern error")
	}
	return *response.NumAffected, nil
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
func streamDocuments(ordered bool, inputChan chan ConvertibleDoc, outputChan chan bson.D, errChan chan error) {
	var importWorkers []*ImportWorker
	// initialize all our concurrent processing threads
	wg := &sync.WaitGroup{}
	inChan := inputChan
	outChan := outputChan
	for i := 0; i < numProcessingThreads; i++ {
		if ordered {
			// TODO: experiment with buffered channel size; the buffer size of
			// inChan should always be the same as that of outChan
			workerBufferSize := 100
			inChan = make(chan ConvertibleDoc, workerBufferSize)
			outChan = make(chan bson.D, workerBufferSize)
		}
		importWorker := &ImportWorker{
			unprocessedDataChan:   inChan,
			processedDocumentChan: outChan,
		}
		importWorkers = append(importWorkers, importWorker)
		wg.Add(1)
		go func() {
			defer wg.Done()
			if err := importWorker.processDocuments(ordered); err != nil {
				errChan <- err
			}
		}()
	}

	// if ordered, we have to coordinate the sequence in which processed
	// documents are passed to the main read channel
	if ordered {
		doSequentialStreaming(importWorkers, inputChan, outputChan)
	}
	wg.Wait()
	close(outputChan)
}

// tokensToBSON reads in slice of records - along with ordered fields names -
// and returns a BSON document for the record.
func tokensToBSON(fields, tokens []string, numProcessed uint64) (bson.D, error) {
	log.Logf(log.DebugLow, "got line: %v", tokens)
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
				return nil, fmt.Errorf("Duplicate header name - on %v - for token #%v ('%v') in document #%v",
					key, index+1, parsedValue, numProcessed)
			}
			document = append(document, bson.DocElem{key, parsedValue})
		}
	}
	return document, nil
}

// validateHeaders takes an InputReader, and does some validation on the
// header fields. It returns an error if an issue is found in the header list
func validateHeaders(inputReader InputReader, hasHeaderLine bool) (validatedFields []string, err error) {
	unsortedHeaders := []string{}
	if hasHeaderLine {
		unsortedHeaders, err = inputReader.ReadHeadersFromSource()
		if err != nil {
			return nil, err
		}
	} else {
		unsortedHeaders = inputReader.GetHeaders()
	}

	headers := make([]string, len(unsortedHeaders), len(unsortedHeaders))
	copy(headers, unsortedHeaders)
	sort.Sort(sort.StringSlice(headers))

	for index, header := range headers {
		if strings.HasSuffix(header, ".") || strings.HasPrefix(header, ".") {
			return nil, fmt.Errorf("header '%v' can not start or end in '.'", header)
		}
		if strings.Contains(header, "..") {
			return nil, fmt.Errorf("header '%v' can not contain consecutive '.' characters", header)
		}
		// NOTE: since headers is sorted, this check ensures that no header
		// is incompatible with another one that occurs further down the list.
		// meant to prevent cases where we have headers like "a" and "a.c"
		for _, latterHeader := range headers[index+1:] {
			// NOTE: this means we will not support imports that have fields that
			// include e.g. a, a.b
			if strings.HasPrefix(latterHeader, header+".") {
				return nil, fmt.Errorf("incompatible headers found: '%v' and '%v",
					header, latterHeader)
			}
			// NOTE: this means we will not support imports that have fields like
			// a, a - since this is invalid in MongoDB
			if header == latterHeader {
				return nil, fmt.Errorf("headers can not be identical: '%v' and '%v",
					header, latterHeader)
			}
		}
		validatedFields = append(validatedFields, unsortedHeaders[index])
	}
	if len(headers) == 1 {
		log.Logf(log.Info, "using field: %v", validatedFields[0])
	} else {
		log.Logf(log.Info, "using fields: %v", strings.Join(validatedFields, ","))
	}
	return validatedFields, nil
}

// processDocuments reads from the ConvertibleDoc channel and for a record,
// converts it to a bson.D document before sending it on the
// processedDocumentChan channel. Once the input channel it closed it closed
// the processed channel if the worker streams its reads in order
func (importWorker *ImportWorker) processDocuments(ordered bool) error {
	for convertibleDoc := range importWorker.unprocessedDataChan {
		document, err := convertibleDoc.Convert()
		if err != nil {
			return err
		}
		importWorker.processedDocumentChan <- document
	}
	if ordered {
		close(importWorker.processedDocumentChan)
	}
	return nil
}
