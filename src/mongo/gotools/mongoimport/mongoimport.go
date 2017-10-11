// Package mongoimport allows importing content from a JSON, CSV, or TSV into a MongoDB instance.
package mongoimport

import (
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"gopkg.in/tomb.v2"

	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
)

// Input format types accepted by mongoimport.
const (
	CSV  = "csv"
	TSV  = "tsv"
	JSON = "json"
)

// Modes accepted by mongoimport.
const (
	modeInsert = "insert"
	modeUpsert = "upsert"
	modeMerge  = "merge"
)

const (
	workerBufferSize  = 16
	progressBarLength = 24
)

// MongoImport is a container for the user-specified options and
// internal state used for running mongoimport.
type MongoImport struct {
	// insertionCount keeps track of how many documents have successfully
	// been inserted into the database
	// updated atomically, aligned at the beginning of the struct
	insertionCount uint64

	// generic mongo tool options
	ToolOptions *options.ToolOptions

	// InputOptions defines options used to read data to be ingested
	InputOptions *InputOptions

	// IngestOptions defines options used to ingest data into MongoDB
	IngestOptions *IngestOptions

	// SessionProvider is used for connecting to the database
	SessionProvider *db.SessionProvider

	// the tomb is used to synchronize ingestion goroutines and causes
	// other sibling goroutines to terminate immediately if one errors out
	tomb.Tomb

	// fields to use for upsert operations
	upsertFields []string

	// type of node the SessionProvider is connected to
	nodeType db.NodeType
}

type InputReader interface {
	// StreamDocument takes a boolean indicating if the documents should be streamed
	// in read order and a channel on which to stream the documents processed from
	// the underlying reader.  Returns a non-nil error if encountered.
	StreamDocument(ordered bool, read chan bson.D) error

	// ReadAndValidateHeader reads the header line from the InputReader and returns
	// a non-nil error if the fields from the header line are invalid; returns
	// nil otherwise. No-op for JSON input readers.
	ReadAndValidateHeader() error

	// ReadAndValidateTypedHeader is the same as ReadAndValidateHeader,
	// except it also parses types from the fields of the header. Parse errors
	// will be handled according parseGrace.
	ReadAndValidateTypedHeader(parseGrace ParseGrace) error

	// embedded io.Reader that tracks number of bytes read, to allow feeding into progress bar.
	sizeTracker
}

// ValidateSettings ensures that the tool specific options supplied for
// MongoImport are valid.
func (imp *MongoImport) ValidateSettings(args []string) error {
	// namespace must have a valid database; if none is specified, use 'test'
	if imp.ToolOptions.DB == "" {
		imp.ToolOptions.DB = "test"
	}
	err := util.ValidateDBName(imp.ToolOptions.DB)
	if err != nil {
		return fmt.Errorf("invalid database name: %v", err)
	}

	imp.InputOptions.Type = strings.ToLower(imp.InputOptions.Type)
	// use JSON as default input type
	if imp.InputOptions.Type == "" {
		imp.InputOptions.Type = JSON
	} else {
		if !(imp.InputOptions.Type == TSV ||
			imp.InputOptions.Type == JSON ||
			imp.InputOptions.Type == CSV) {
			return fmt.Errorf("unknown type %v", imp.InputOptions.Type)
		}
	}

	// ensure headers are supplied for CSV/TSV
	if imp.InputOptions.Type == CSV ||
		imp.InputOptions.Type == TSV {
		if !imp.InputOptions.HeaderLine {
			if imp.InputOptions.Fields == nil &&
				imp.InputOptions.FieldFile == nil {
				return fmt.Errorf("must specify --fields, --fieldFile or --headerline to import this file type")
			}
			if imp.InputOptions.FieldFile != nil &&
				*imp.InputOptions.FieldFile == "" {
				return fmt.Errorf("--fieldFile can not be empty string")
			}
			if imp.InputOptions.Fields != nil &&
				imp.InputOptions.FieldFile != nil {
				return fmt.Errorf("incompatible options: --fields and --fieldFile")
			}
		} else {
			if imp.InputOptions.Fields != nil {
				return fmt.Errorf("incompatible options: --fields and --headerline")
			}
			if imp.InputOptions.FieldFile != nil {
				return fmt.Errorf("incompatible options: --fieldFile and --headerline")
			}
		}

		if _, err := ValidatePG(imp.InputOptions.ParseGrace); err != nil {
			return err
		}
	} else {
		// input type is JSON
		if imp.InputOptions.HeaderLine {
			return fmt.Errorf("can not use --headerline when input type is JSON")
		}
		if imp.InputOptions.Fields != nil {
			return fmt.Errorf("can not use --fields when input type is JSON")
		}
		if imp.InputOptions.FieldFile != nil {
			return fmt.Errorf("can not use --fieldFile when input type is JSON")
		}
		if imp.IngestOptions.IgnoreBlanks {
			return fmt.Errorf("can not use --ignoreBlanks when input type is JSON")
		}
		if imp.InputOptions.ColumnsHaveTypes {
			return fmt.Errorf("can not use --columnsHaveTypes when input type is JSON")
		}
	}

	// deprecated
	if imp.IngestOptions.Upsert == true {
		imp.IngestOptions.Mode = modeUpsert
	}

	// parse UpsertFields, may set default mode to modeUpsert
	if imp.IngestOptions.UpsertFields != "" {
		if imp.IngestOptions.Mode == "" {
			imp.IngestOptions.Mode = modeUpsert
		} else if imp.IngestOptions.Mode == modeInsert {
			return fmt.Errorf("can not use --upsertFields with --mode=insert")
		}
		imp.upsertFields = strings.Split(imp.IngestOptions.UpsertFields, ",")
		if err := validateFields(imp.upsertFields); err != nil {
			return fmt.Errorf("invalid --upsertFields argument: %v", err)
		}
	} else if imp.IngestOptions.Mode != modeInsert {
		imp.upsertFields = []string{"_id"}
	}

	// set default mode, must be after parsing UpsertFields
	if imp.IngestOptions.Mode == "" {
		imp.IngestOptions.Mode = modeInsert
	}

	// double-check mode choices
	if !(imp.IngestOptions.Mode == modeInsert ||
		imp.IngestOptions.Mode == modeUpsert ||
		imp.IngestOptions.Mode == modeMerge) {
		return fmt.Errorf("invalid --mode argument: %v", imp.IngestOptions.Mode)
	}

	if imp.IngestOptions.Mode != modeInsert {
		imp.IngestOptions.MaintainInsertionOrder = true
		log.Logvf(log.Info, "using upsert fields: %v", imp.upsertFields)
	}

	// set the number of decoding workers to use for imports
	if imp.IngestOptions.NumDecodingWorkers <= 0 {
		imp.IngestOptions.NumDecodingWorkers = imp.ToolOptions.MaxProcs
	}
	log.Logvf(log.DebugLow, "using %v decoding workers", imp.IngestOptions.NumDecodingWorkers)

	// set the number of insertion workers to use for imports
	if imp.IngestOptions.NumInsertionWorkers <= 0 {
		imp.IngestOptions.NumInsertionWorkers = 1
	}

	log.Logvf(log.DebugLow, "using %v insert workers", imp.IngestOptions.NumInsertionWorkers)

	// if --maintainInsertionOrder is set, we can only allow 1 insertion worker
	if imp.IngestOptions.MaintainInsertionOrder {
		imp.IngestOptions.NumInsertionWorkers = 1
	}

	// get the number of documents per batch
	if imp.IngestOptions.BulkBufferSize <= 0 || imp.IngestOptions.BulkBufferSize > 1000 {
		imp.IngestOptions.BulkBufferSize = 1000
	}

	// ensure no more than one positional argument is supplied
	if len(args) > 1 {
		return fmt.Errorf("only one positional argument is allowed")
	}

	// ensure either a positional argument is supplied or an argument is passed
	// to the --file flag - and not both
	if imp.InputOptions.File != "" && len(args) != 0 {
		return fmt.Errorf("incompatible options: --file and positional argument(s)")
	}

	if imp.InputOptions.File == "" {
		if len(args) != 0 {
			// if --file is not supplied, use the positional argument supplied
			imp.InputOptions.File = args[0]
		}
	}

	// ensure we have a valid string to use for the collection
	if imp.ToolOptions.Collection == "" {
		log.Logvf(log.Always, "no collection specified")
		fileBaseName := filepath.Base(imp.InputOptions.File)
		lastDotIndex := strings.LastIndex(fileBaseName, ".")
		if lastDotIndex != -1 {
			fileBaseName = fileBaseName[0:lastDotIndex]
		}
		log.Logvf(log.Always, "using filename '%v' as collection", fileBaseName)
		imp.ToolOptions.Collection = fileBaseName
	}
	err = util.ValidateCollectionName(imp.ToolOptions.Collection)
	if err != nil {
		return fmt.Errorf("invalid collection name: %v", err)
	}
	return nil
}

// getSourceReader returns an io.Reader to read from the input source. Also
// returns a progress.Progressor which can be used to track progress if the
// reader supports it.
func (imp *MongoImport) getSourceReader() (io.ReadCloser, int64, error) {
	if imp.InputOptions.File != "" {
		file, err := os.Open(util.ToUniversalPath(imp.InputOptions.File))
		if err != nil {
			return nil, -1, err
		}
		fileStat, err := file.Stat()
		if err != nil {
			return nil, -1, err
		}
		log.Logvf(log.Info, "filesize: %v bytes", fileStat.Size())
		return file, int64(fileStat.Size()), err
	}

	log.Logvf(log.Info, "reading from stdin")

	// Stdin has undefined max size, so return 0
	return os.Stdin, 0, nil
}

// fileSizeProgressor implements Progressor to allow a sizeTracker to hook up with a
// progress.Bar instance, so that the progress bar can report the percentage of the file read.
type fileSizeProgressor struct {
	max int64
	sizeTracker
}

func (fsp *fileSizeProgressor) Progress() (int64, int64) {
	return fsp.sizeTracker.Size(), fsp.max
}

// ImportDocuments is used to write input data to the database. It returns the
// number of documents successfully imported to the appropriate namespace and
// any error encountered in doing this
func (imp *MongoImport) ImportDocuments() (uint64, error) {
	source, fileSize, err := imp.getSourceReader()
	if err != nil {
		return 0, err
	}
	defer source.Close()

	inputReader, err := imp.getInputReader(source)
	if err != nil {
		return 0, err
	}

	if imp.InputOptions.HeaderLine {
		if imp.InputOptions.ColumnsHaveTypes {
			err = inputReader.ReadAndValidateTypedHeader(ParsePG(imp.InputOptions.ParseGrace))
		} else {
			err = inputReader.ReadAndValidateHeader()
		}
		if err != nil {
			return 0, err
		}
	}

	bar := &progress.Bar{
		Name:      fmt.Sprintf("%v.%v", imp.ToolOptions.DB, imp.ToolOptions.Collection),
		Watching:  &fileSizeProgressor{fileSize, inputReader},
		Writer:    log.Writer(0),
		BarLength: progressBarLength,
		IsBytes:   true,
	}
	bar.Start()
	defer bar.Stop()
	return imp.importDocuments(inputReader)
}

// importDocuments is a helper to ImportDocuments and does all the ingestion
// work by taking data from the inputReader source and writing it to the
// appropriate namespace
func (imp *MongoImport) importDocuments(inputReader InputReader) (numImported uint64, retErr error) {
	session, err := imp.SessionProvider.GetSession()
	if err != nil {
		return 0, err
	}
	defer session.Close()

	connURL := imp.ToolOptions.Host
	if connURL == "" {
		connURL = util.DefaultHost
	}
	if imp.ToolOptions.Port != "" {
		connURL = connURL + ":" + imp.ToolOptions.Port
	}
	log.Logvf(log.Always, "connected to: %v", connURL)

	log.Logvf(log.Info, "ns: %v.%v",
		imp.ToolOptions.Namespace.DB,
		imp.ToolOptions.Namespace.Collection)

	// check if the server is a replica set, mongos, or standalone
	imp.nodeType, err = imp.SessionProvider.GetNodeType()
	if err != nil {
		return 0, fmt.Errorf("error checking connected node type: %v", err)
	}
	log.Logvf(log.Info, "connected to node type: %v", imp.nodeType)

	if err = imp.configureSession(session); err != nil {
		return 0, fmt.Errorf("error configuring session: %v", err)
	}

	// drop the database if necessary
	if imp.IngestOptions.Drop {
		log.Logvf(log.Always, "dropping: %v.%v",
			imp.ToolOptions.DB,
			imp.ToolOptions.Collection)
		collection := session.DB(imp.ToolOptions.DB).
			C(imp.ToolOptions.Collection)
		if err := collection.DropCollection(); err != nil {
			if err.Error() != db.ErrNsNotFound {
				return 0, err
			}
		}
	}

	readDocs := make(chan bson.D, workerBufferSize)
	processingErrChan := make(chan error)
	ordered := imp.IngestOptions.MaintainInsertionOrder

	// read and process from the input reader
	go func() {
		processingErrChan <- inputReader.StreamDocument(ordered, readDocs)
	}()

	// insert documents into the target database
	go func() {
		processingErrChan <- imp.ingestDocuments(readDocs)
	}()

	e1 := channelQuorumError(processingErrChan, 2)
	insertionCount := atomic.LoadUint64(&imp.insertionCount)
	return insertionCount, e1
}

// ingestDocuments accepts a channel from which it reads documents to be inserted
// into the target collection. It spreads the insert/upsert workload across one
// or more workers.
func (imp *MongoImport) ingestDocuments(readDocs chan bson.D) (retErr error) {
	numInsertionWorkers := imp.IngestOptions.NumInsertionWorkers
	if numInsertionWorkers <= 0 {
		numInsertionWorkers = 1
	}

	// Each ingest worker will return an error which will
	// be set in the following cases:
	//
	// 1. There is a problem connecting with the server
	// 2. The server becomes unreachable
	// 3. There is an insertion/update error - e.g. duplicate key
	//    error - and stopOnError is set to true

	wg := new(sync.WaitGroup)
	for i := 0; i < numInsertionWorkers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			// only set the first insertion error and cause sibling goroutines to terminate immediately
			err := imp.runInsertionWorker(readDocs)
			if err != nil && retErr == nil {
				retErr = err
				imp.Kill(err)
			}
		}()
	}
	wg.Wait()
	return
}

// configureSession takes in a session and modifies it with properly configured
// settings. It does the following configurations:
//
// 1. Sets the session to not timeout
// 2. Sets the write concern on the session
// 3. Sets the session safety
//
// returns an error if it's unable to set the write concern
func (imp *MongoImport) configureSession(session *mgo.Session) error {
	// sockets to the database will never be forcibly closed
	session.SetSocketTimeout(0)
	sessionSafety, err := db.BuildWriteConcern(imp.IngestOptions.WriteConcern, imp.nodeType,
		imp.ToolOptions.ParsedConnString())
	if err != nil {
		return fmt.Errorf("write concern error: %v", err)
	}
	session.SetSafe(sessionSafety)

	return nil
}

type flushInserter interface {
	Insert(doc interface{}) error
	Flush() error
}

// runInsertionWorker is a helper to InsertDocuments - it reads document off
// the read channel and prepares then in batches for insertion into the databas
func (imp *MongoImport) runInsertionWorker(readDocs chan bson.D) (err error) {
	session, err := imp.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error connecting to mongod: %v", err)
	}
	defer session.Close()
	if err = imp.configureSession(session); err != nil {
		return fmt.Errorf("error configuring session: %v", err)
	}
	collection := session.DB(imp.ToolOptions.DB).C(imp.ToolOptions.Collection)

	var inserter flushInserter
	if imp.IngestOptions.Mode == modeInsert {
		inserter = db.NewBufferedBulkInserter(collection, imp.IngestOptions.BulkBufferSize, !imp.IngestOptions.StopOnError)
		if !imp.IngestOptions.MaintainInsertionOrder {
			inserter.(*db.BufferedBulkInserter).Unordered()
		}
	} else {
		inserter = imp.newUpserter(collection)
	}

readLoop:
	for {
		select {
		case document, alive := <-readDocs:
			if !alive {
				break readLoop
			}
			err = filterIngestError(imp.IngestOptions.StopOnError, inserter.Insert(document))
			if err != nil {
				return err
			}
			atomic.AddUint64(&imp.insertionCount, 1)
		case <-imp.Dying():
			return nil
		}
	}

	err = inserter.Flush()
	// TOOLS-349 correct import count for bulk operations
	if bulkError, ok := err.(*mgo.BulkError); ok {
		failedDocs := make(map[int]bool) // index of failures
		for _, failure := range bulkError.Cases() {
			failedDocs[failure.Index] = true
		}
		numFailures := len(failedDocs)
		if numFailures > 0 {
			log.Logvf(log.Always, "num failures: %d", numFailures)
			atomic.AddUint64(&imp.insertionCount, ^uint64(numFailures-1))
		}
	}
	return filterIngestError(imp.IngestOptions.StopOnError, err)
}

type upserter struct {
	imp        *MongoImport
	collection *mgo.Collection
}

func (imp *MongoImport) newUpserter(collection *mgo.Collection) *upserter {
	return &upserter{
		imp:        imp,
		collection: collection,
	}
}

// Insert is part of the flushInserter interface and performs
// upserts or inserts.
func (up *upserter) Insert(doc interface{}) error {
	document := doc.(bson.D)
	selector := constructUpsertDocument(up.imp.upsertFields, document)
	var err error
	if selector == nil { // modeInsert || doc-not-exist
		err = up.collection.Insert(document)
	} else if up.imp.IngestOptions.Mode == modeUpsert {
		_, err = up.collection.Upsert(selector, document)
	} else { // modeMerge
		_, err = up.collection.Upsert(selector, bson.M{"$set": document})
	}
	return err
}

// Flush is needed so that upserter implements flushInserter, but upserter
// doesn't buffer anything so we don't need to do anything in Flush.
func (up *upserter) Flush() error {
	return nil
}

func splitInlineHeader(header string) (headers []string) {
	var level uint8
	var currentField string
	for _, c := range header {
		if c == '(' {
			level++
		} else if c == ')' && level > 0 {
			level--
		}
		if c == ',' && level == 0 {
			headers = append(headers, currentField)
			currentField = ""
		} else {
			currentField = currentField + string(c)
		}
	}
	headers = append(headers, currentField) // add last field
	return
}

// getInputReader returns an implementation of InputReader based on the input type
func (imp *MongoImport) getInputReader(in io.Reader) (InputReader, error) {
	var colSpecs []ColumnSpec
	var headers []string
	var err error
	if imp.InputOptions.Fields != nil {
		headers = splitInlineHeader(*imp.InputOptions.Fields)
	} else if imp.InputOptions.FieldFile != nil {
		headers, err = util.GetFieldsFromFile(*imp.InputOptions.FieldFile)
		if err != nil {
			return nil, err
		}
	}
	if imp.InputOptions.ColumnsHaveTypes {
		colSpecs, err = ParseTypedHeaders(headers, ParsePG(imp.InputOptions.ParseGrace))
		if err != nil {
			return nil, err
		}
	} else {
		colSpecs = ParseAutoHeaders(headers)
	}

	// header fields validation can only happen once we have an input reader
	if !imp.InputOptions.HeaderLine {
		if err = validateReaderFields(ColumnNames(colSpecs)); err != nil {
			return nil, err
		}
	}

	out := os.Stdout

	ignoreBlanks := imp.IngestOptions.IgnoreBlanks && imp.InputOptions.Type != JSON
	if imp.InputOptions.Type == CSV {
		return NewCSVInputReader(colSpecs, in, out, imp.IngestOptions.NumDecodingWorkers, ignoreBlanks), nil
	} else if imp.InputOptions.Type == TSV {
		return NewTSVInputReader(colSpecs, in, out, imp.IngestOptions.NumDecodingWorkers, ignoreBlanks), nil
	}
	return NewJSONInputReader(imp.InputOptions.JSONArray, in, imp.IngestOptions.NumDecodingWorkers), nil
}
