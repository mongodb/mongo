package mongoimport

import (
	"errors"
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	commonOpts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongoimport/options"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
)

// input type constants
const (
	CSV  = "csv"
	TSV  = "tsv"
	JSON = "json"
)

// known errors to look out for - we immediately
// terminate mongoimport if we get any of these errors
var (
	errNsNotFound        = errors.New("ns not found")
	errNoReachableServer = errors.New("no reachable servers")
)

// ingestion constants
const (
	maxBSONSize         = 16 * (1024 * 1024)
	maxMessageSizeBytes = 2 * maxBSONSize
)

// variables used by the input/ingestion goroutines
var (
	numProcessingThreads = 1
	batchSize            = 1
)

// Wrapper for MongoImport functionality
type MongoImport struct {
	// generic mongo tool options
	ToolOptions *commonOpts.ToolOptions

	// InputOptions defines options used to read data to be ingested
	InputOptions *options.InputOptions

	// IngestOptions defines options used to ingest data into MongoDB
	IngestOptions *options.IngestOptions

	// SessionProvider is used for connecting to the database
	SessionProvider *db.SessionProvider

	// insertionLock is used to prevent race conditions in incrementing
	// the insertion count
	insertionLock *sync.Mutex

	// insertionCount keeps track of how many documents have successfully
	// been inserted into the database
	insertionCount uint64
}

// InputReader is an interface that specifies how an input source should be
// converted to BSON
type InputReader interface {
	// StreamDocument reads the given record from the given io.Reader according
	// to the format supported by the underlying InputReader implementation. It
	// returns the documents read on the readChannel channel and also sends any
	// errors it encounters on the errorChannel channel. If ordered is true, it
	// streams document in the order in which they are read from the reader
	StreamDocument(ordered bool, readChannel chan bson.D, errorChannel chan error)

	// SetHeader sets the header for the CSV/TSV import when --headerline is
	// specified. It a --fields or --fieldFile argument is passed, it overwrites
	// the values of those with what is read from the input source
	SetHeader(bool) error

	// ReadHeadersFromSource attempts to reads the header fields for the
	// specific implementation
	ReadHeadersFromSource() ([]string, error)

	// GetHeaders returns the current set of header fields for the specific
	// implementation
	GetHeaders() []string
}

// ValidateSettings ensures that the tool specific options supplied for
// MongoImport are valid
func (mongoImport *MongoImport) ValidateSettings(args []string) error {
	if err := mongoImport.ToolOptions.Validate(); err != nil {
		return err
	}

	// TODO: move to common
	// --dbpath is now deprecated for tools with version >= v2.8
	if mongoImport.ToolOptions.DBPath != "" {
		return fmt.Errorf("--dbpath is now deprecated. start a mongod instead")
	}

	// Namespace must have a valid database if none is specified,
	// use 'test'
	if mongoImport.ToolOptions.Namespace.DB == "" {
		mongoImport.ToolOptions.Namespace.DB = "test"
	} else {
		err := util.ValidateDBName(mongoImport.ToolOptions.Namespace.DB)
		if err != nil {
			return err
		}
	}

	// use JSON as default input type
	if mongoImport.InputOptions.Type == "" {
		mongoImport.InputOptions.Type = JSON
	} else {
		if !(mongoImport.InputOptions.Type == TSV ||
			mongoImport.InputOptions.Type == JSON ||
			mongoImport.InputOptions.Type == CSV) {
			return fmt.Errorf("don't know what type [\"%v\"] is",
				mongoImport.InputOptions.Type)
		}
	}

	// ensure headers are supplied for CSV/TSV
	if mongoImport.InputOptions.Type == CSV ||
		mongoImport.InputOptions.Type == TSV {
		if !mongoImport.InputOptions.HeaderLine {
			if mongoImport.InputOptions.Fields == "" &&
				mongoImport.InputOptions.FieldFile == "" {
				return fmt.Errorf("You need to specify fields or have a " +
					"header line to import this file type")
			}
			if mongoImport.InputOptions.Fields != "" &&
				mongoImport.InputOptions.FieldFile != "" {
				return fmt.Errorf("incompatible options: --fields and --fieldFile")
			}
		} else {
			if mongoImport.InputOptions.Fields != "" {
				return fmt.Errorf("incompatible options: --fields and --headerline")
			}
			if mongoImport.InputOptions.FieldFile != "" {
				return fmt.Errorf("incompatible options: --fieldFile and --headerline")
			}
		}
	}

	numCPU := runtime.NumCPU()

	// set the number of operating system threads to use for imports
	if mongoImport.IngestOptions.NumOSThreads == nil {
		runtime.GOMAXPROCS(numCPU)
	} else {
		if *mongoImport.IngestOptions.NumOSThreads < 1 {
			return fmt.Errorf("--numOSThreads argument must be > 0")
		}
		runtime.GOMAXPROCS(*mongoImport.IngestOptions.NumOSThreads)
	}

	// set the number of processing threads to use for imports
	if mongoImport.IngestOptions.NumProcessingThreads == nil {
		numProcessingThreads = numCPU
		mongoImport.IngestOptions.NumProcessingThreads = &numCPU
	} else {
		if *mongoImport.IngestOptions.NumProcessingThreads < 1 {
			return fmt.Errorf("--numProcessingThreads argument must be > 0")
		}
		numProcessingThreads = *mongoImport.IngestOptions.NumProcessingThreads
	}

	// set the number of ingestion threads to use for imports
	if mongoImport.IngestOptions.NumIngestionThreads == nil {
		mongoImport.IngestOptions.NumIngestionThreads = &numCPU
	} else if *mongoImport.IngestOptions.NumIngestionThreads < 1 {
		return fmt.Errorf("--numIngestionThreads argument must be > 0")
	}

	// if maintain --maintainInsertionOrder is true, we can only have one
	// ingestion thread
	if mongoImport.IngestOptions.MaintainInsertionOrder {
		*mongoImport.IngestOptions.NumIngestionThreads = 1
	}

	// ensure no more than one positional argument is supplied
	if len(args) > 1 {
		return fmt.Errorf("only one positional argument is allowed")
	}

	// ensure either a positional argument is supplied or an argument is passed
	// to the --file flag - and not both
	if mongoImport.InputOptions.File != "" && len(args) != 0 {
		return fmt.Errorf("incompatible options: --file and positional argument(s)")
	}

	var fileBaseName string

	if mongoImport.InputOptions.File != "" {
		fileBaseName = mongoImport.InputOptions.File
	} else {
		if len(args) != 0 {
			fileBaseName = args[0]
			mongoImport.InputOptions.File = fileBaseName
		}
	}

	// ensure we have a valid string to use for the collection
	if mongoImport.ToolOptions.Namespace.Collection == "" {
		if fileBaseName == "" {
			return fmt.Errorf("no collection specified")
		}
		fileBaseName = filepath.Base(fileBaseName)
		if lastDotIndex := strings.LastIndex(fileBaseName, "."); lastDotIndex != -1 {
			fileBaseName = fileBaseName[0:lastDotIndex]
		}
		if err := util.ValidateCollectionName(fileBaseName); err != nil {
			return err
		}
		mongoImport.ToolOptions.Namespace.Collection = fileBaseName
		log.Logf(0, "no collection specified")
		log.Logf(0, "using filename '%v' as collection",
			mongoImport.ToolOptions.Namespace.Collection)
	}
	return nil
}

// getSourceReader returns an io.Reader to read from the input source
func (mongoImport *MongoImport) getSourceReader() (io.ReadCloser, error) {
	if mongoImport.InputOptions.File != "" {
		file, err := os.Open(util.ToUniversalPath(mongoImport.InputOptions.File))
		if err != nil {
			return nil, err
		}
		fileStat, err := file.Stat()
		if err != nil {
			return nil, err
		}
		log.Logf(1, "filesize: %v", fileStat.Size())
		return file, err
	}
	log.Logf(1, "filesize: 0")
	return os.Stdin, nil
}

// ImportDocuments is used to write input data to the database. It returns the
// number of documents successfully imported to the appropriate namespace and
// any error encountered in doing this
func (mongoImport *MongoImport) ImportDocuments() (uint64, error) {
	source, err := mongoImport.getSourceReader()
	if err != nil {
		return 0, err
	}
	defer source.Close()

	inputReader, err := mongoImport.getInputReader(source)
	if err != nil {
		return 0, err
	}

	err = inputReader.SetHeader(mongoImport.InputOptions.HeaderLine)
	if err != nil {
		return 0, err
	}
	return mongoImport.importDocuments(inputReader)
}

// importDocuments is a helper to ImportDocuments and does all the ingestion
// work by taking data from the inputReader source and writing it to the
// appropriate namespace
func (mongoImport *MongoImport) importDocuments(inputReader InputReader) (numImported uint64, retErr error) {
	connURL := mongoImport.ToolOptions.Host
	if connURL == "" {
		connURL = util.DefaultHost
	}
	var readErr error
	session, err := mongoImport.SessionProvider.GetSession()
	if err != nil {
		return 0, fmt.Errorf("error connecting to mongod: %v", err)
	}
	defer func() {
		session.Close()
		if readErr != nil && readErr == io.EOF {
			readErr = nil
		}
		if retErr == nil {
			retErr = readErr
		}
	}()

	if mongoImport.ToolOptions.Port != "" {
		connURL = connURL + ":" + mongoImport.ToolOptions.Port
	}
	log.Logf(0, "connected to: %v", connURL)

	log.Logf(1, "ns: %v.%v",
		mongoImport.ToolOptions.Namespace.DB,
		mongoImport.ToolOptions.Namespace.Collection)

	// drop the database if necessary
	if mongoImport.IngestOptions.Drop {
		log.Logf(0, "dropping: %v.%v",
			mongoImport.ToolOptions.DB,
			mongoImport.ToolOptions.Collection)
		collection := session.DB(mongoImport.ToolOptions.DB).
			C(mongoImport.ToolOptions.Collection)
		if err := collection.DropCollection(); err != nil {
			// TODO: do all mongods (e.g. v2.4) return this same
			// error message?
			if err.Error() != errNsNotFound.Error() {
				return 0, err
			}
		}
	}

	// determine whether or not documents should be streamed in read order
	ordered := mongoImport.IngestOptions.MaintainInsertionOrder

	// set the batch size for ingestion
	batchSize = *mongoImport.IngestOptions.BatchSize

	// set the number of processing threads and batch size
	readDocChanSize := *mongoImport.IngestOptions.BatchSize *
		*mongoImport.IngestOptions.NumProcessingThreads

	// readDocChan is buffered with readDocChanSize to ensure we only block
	// accepting reads if processing is slow
	readDocChan := make(chan bson.D, readDocChanSize)

	// any read errors should cause mongoimport to stop
	// ingestion and immediately terminate; thus, we
	// leave this channel unbuffered
	readErrChan := make(chan error)

	// handle all input reads in a separate goroutine
	go inputReader.StreamDocument(ordered, readDocChan, readErrChan)

	// initialize insertion lock
	mongoImport.insertionLock = &sync.Mutex{}

	// return immediately on ingest errors - these will be triggered
	// either by an issue ingesting data or if the read channel is
	// closed so we can block here while reads happen in a goroutine
	if err = mongoImport.IngestDocuments(readDocChan); err != nil {
		return mongoImport.insertionCount, err
	}
	readErr = <-readErrChan
	return mongoImport.insertionCount, retErr
}

// IngestDocuments takes a slice of documents and either inserts/upserts them -
// based on whether an upsert is requested - into the given collection
func (mongoImport *MongoImport) IngestDocuments(readChan chan bson.D) (err error) {
	numIngestionThreads := *mongoImport.IngestOptions.NumIngestionThreads
	ingestErr := make(chan error, numIngestionThreads)

	// spawn all the worker threads, each in its own goroutine
	for i := 0; i < numIngestionThreads; i++ {
		go func() {
			ingestErr <- mongoImport.ingestDocs(readChan)
		}()
	}
	doneIngestionThreads := 0

	// Each ingest worker will return an error which may
	// be nil or not. It will be not nil in any of this cases:
	//
	// 1. There is a problem connecting with the server
	// 2. There server becomes unreachable
	// 3. There is an insertion/update error - e.g. duplicate key
	//    error - and stopOnError is set to true

	for err = range ingestErr {
		doneIngestionThreads++
		// an error in any goroutine will immediately be propagated to the
		// and will cause the program to exit so we don't terminate the other
		// ingestion threads here
		if err != nil {
			return
		}
		if doneIngestionThreads == numIngestionThreads {
			return
		}
	}
	return
}

// ingestDocuments is a helper to IngestDocuments - it reads document off the
// read channel and prepares then for insertion into the database
func (mongoImport *MongoImport) ingestDocs(readChan chan bson.D) (err error) {
	ignoreBlanks := mongoImport.IngestOptions.IgnoreBlanks && mongoImport.InputOptions.Type != JSON
	documentBytes := make([]byte, 0)
	documents := make([]interface{}, 0)
	numMessageBytes := 0

	// TODO: mgo driver does not reestablish connections once lost
	session, err := mongoImport.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error connecting to mongod: %v", err)
	}

	// sockets to the database will never be forcibly closed
	session.SetSocketTimeout(0)

	// set write concern if specified by user; if no write concern
	// is specified, use "majority"
	safety := &mgo.Safe{}
	if mongoImport.IngestOptions.WriteConcern == nil {
		safety.WMode = "majority"
	} else {
		safety.W = *mongoImport.IngestOptions.WriteConcern
	}
	session.SetSafe(safety)

	defer session.Close()

	collection := session.DB(mongoImport.ToolOptions.DB).
		C(mongoImport.ToolOptions.Collection)

	var document bson.D
	for document = range readChan {
		// ignore blank fields if specified
		if ignoreBlanks {
			document = removeBlankFields(document)
		}
		if documentBytes, err = bson.Marshal(document); err != nil {
			return err
		}
		numMessageBytes += len(documentBytes)
		documents = append(documents, bson.Raw{3, documentBytes})

		// the mgo driver doesn't currently respect the maxBatchSize
		// limit so we limit ourselves by using maxMessageSizeBytes
		// send documents over the wire when we hit the batch size
		// or are at/over the maximum message size threshold
		if len(documents) == batchSize ||
			numMessageBytes >= maxMessageSizeBytes {
			if err = mongoImport.ingester(documents, collection); err != nil {
				return err
			}
			if mongoImport.insertionCount%10000 == 0 {
				log.Logf(0, "Progress: %v documents inserted...", mongoImport.insertionCount)
			}
			documents = documents[:0]
			numMessageBytes = 0
		}
	}

	// ingest any documents left in slice
	if len(documents) != 0 {
		return mongoImport.ingester(documents, collection)
	}
	return nil
}

// ingester performs the actual insertion/updates. If no upsert fields are
// present in the document to be inserted, it simply inserts the documents
// into the given collection
func (mongoImport *MongoImport) ingester(documents []interface{}, collection *mgo.Collection) (err error) {
	// TODO: need a way of doing ordered/unordered
	// bulk updates using write commands
	if mongoImport.IngestOptions.Upsert {
		selector := bson.M{}
		upsertFields := strings.Split(mongoImport.IngestOptions.UpsertFields, ",")
		for _, d := range documents {
			var document bson.M
			if err = bson.Unmarshal(d.(bson.Raw).Data, &document); err != nil {
				return fmt.Errorf("error unmarshaling document: %v", err)
			}
			selector = constructUpsertDocument(upsertFields, document)
			if selector == nil {
				err = collection.Insert(document)
			} else {
				_, err = collection.Upsert(selector, document)
			}
			if err != nil {
				// Need to ascertain what kinds of errors the mgo
				// driver returns for both insert/upsert operations
				if mongoImport.IngestOptions.StopOnError ||
					err.Error() == errNoReachableServer.Error() {
					return err
				}
				log.Logf(0, "error inserting documents: %v", err)
			}
		}
	} else {
		bulk := collection.Bulk()
		bulk.Insert(documents...)
		if !mongoImport.IngestOptions.MaintainInsertionOrder {
			bulk.Unordered()
		}
		// mgo.Bulk doesn't currently implement write commands so
		// mgo.BulkResult isn't informative
		if _, err = bulk.Run(); err != nil {
			if mongoImport.IngestOptions.StopOnError ||
				err.Error() == errNoReachableServer.Error() {
				return err
			}
			log.Logf(0, "error inserting documents: %v", err)
		}
	}

	// TODO: what if only some documents were inserted? need
	// to improve the bulk insert API support in mgo driver
	mongoImport.insertionLock.Lock()
	mongoImport.insertionCount += uint64(len(documents))
	mongoImport.insertionLock.Unlock()
	return nil
}

// getInputReader returns an implementation of InputReader which can handle
// transforming TSV, CSV, or JSON into appropriate BSON documents
func (mongoImport *MongoImport) getInputReader(in io.Reader) (InputReader, error) {
	var fields []string
	var err error
	if len(mongoImport.InputOptions.Fields) != 0 {
		fields = strings.Split(strings.Trim(mongoImport.InputOptions.Fields, " "), ",")
	} else if mongoImport.InputOptions.FieldFile != "" {
		fields, err = util.GetFieldsFromFile(mongoImport.InputOptions.FieldFile)
		if err != nil {
			return nil, err
		}
	}
	if mongoImport.InputOptions.Type == CSV {
		return NewCSVInputReader(fields, in), nil
	} else if mongoImport.InputOptions.Type == TSV {
		return NewTSVInputReader(fields, in), nil
	}
	return NewJSONInputReader(mongoImport.InputOptions.JSONArray, in), nil
}
