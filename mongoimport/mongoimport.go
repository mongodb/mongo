package mongoimport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"gopkg.in/tomb.v2"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// input type constants
const (
	CSV  = "csv"
	TSV  = "tsv"
	JSON = "json"
)

// ingestion constants
const (
	maxBSONSize         = 16 * (1024 * 1024)
	maxMessageSizeBytes = 2 * maxBSONSize
	workerBufferSize    = 16
)

// Wrapper for MongoImport functionality
type MongoImport struct {
	// generic mongo tool options
	ToolOptions *options.ToolOptions

	// InputOptions defines options used to read data to be ingested
	InputOptions *InputOptions

	// IngestOptions defines options used to ingest data into MongoDB
	IngestOptions *IngestOptions

	// SessionProvider is used for connecting to the database
	SessionProvider *db.SessionProvider

	// indicates whether the connected server is part of a replica set
	isReplicaSet bool

	// insertionLock is used to prevent race conditions in incrementing
	// the insertion count
	insertionLock sync.Mutex

	// insertionCount keeps track of how many documents have successfully
	// been inserted into the database
	insertionCount uint64

	// the tomb is used to synchronize ingestion goroutines and causes
	// other sibling goroutines to terminate immediately if one errors out
	tomb.Tomb

	// fields to use for upsert operations
	upsertFields []string
}

type InputReader interface {
	// StreamDocument sends InputReader processed BSON documents on the read channel
	// If an error is encountered during processing, it is sent on the err channel.
	// Otherwise, once documents are streamed, nil is sent on the err channel.
	StreamDocument(ordered bool, read chan bson.D, err chan error)

	// ReadAndValidateHeader reads the header line from the InputReader and returns
	// a non-nil error if the fields from the header line are invalid; returns
	// nil otherwise. No-op for JSON input readers
	ReadAndValidateHeader() error

	// embedded io.Reader that tracks number of bytes read, to allow feeding into progress bar
	sizeTracker
}

// ValidateSettings ensures that the tool specific options supplied for
// MongoImport are valid
func (mongoImport *MongoImport) ValidateSettings(args []string) error {
	// namespace must have a valid database; if none is specified, use 'test'
	if mongoImport.ToolOptions.DB == "" {
		mongoImport.ToolOptions.DB = "test"
	}
	err := util.ValidateDBName(mongoImport.ToolOptions.DB)
	if err != nil {
		return fmt.Errorf("invalid database name: %v", err)
	}

	// use JSON as default input type
	if mongoImport.InputOptions.Type == "" {
		mongoImport.InputOptions.Type = JSON
	} else {
		if !(mongoImport.InputOptions.Type == TSV ||
			mongoImport.InputOptions.Type == JSON ||
			mongoImport.InputOptions.Type == CSV) {
			return fmt.Errorf("unknown type %v", mongoImport.InputOptions.Type)
		}
	}

	// ensure headers are supplied for CSV/TSV
	if mongoImport.InputOptions.Type == CSV ||
		mongoImport.InputOptions.Type == TSV {
		if !mongoImport.InputOptions.HeaderLine {
			if mongoImport.InputOptions.Fields == nil &&
				mongoImport.InputOptions.FieldFile == nil {
				return fmt.Errorf("must specify --fields, --fieldFile or --headerline to import this file type")
			}
			if mongoImport.InputOptions.FieldFile != nil &&
				*mongoImport.InputOptions.FieldFile == "" {
				return fmt.Errorf("--fieldFile can not be empty string")
			}
			if mongoImport.InputOptions.Fields != nil &&
				mongoImport.InputOptions.FieldFile != nil {
				return fmt.Errorf("incompatible options: --fields and --fieldFile")
			}
		} else {
			if mongoImport.InputOptions.Fields != nil {
				return fmt.Errorf("incompatible options: --fields and --headerline")
			}
			if mongoImport.InputOptions.FieldFile != nil {
				return fmt.Errorf("incompatible options: --fieldFile and --headerline")
			}
		}
	} else {
		// input type is JSON
		if mongoImport.InputOptions.HeaderLine {
			return fmt.Errorf("can not use --headerline when input type is JSON")
		}
		if mongoImport.InputOptions.Fields != nil {
			return fmt.Errorf("can not use --fields when input type is JSON")
		}
		if mongoImport.InputOptions.FieldFile != nil {
			return fmt.Errorf("can not use --fieldFile when input type is JSON")
		}
		if mongoImport.IngestOptions.IgnoreBlanks {
			return fmt.Errorf("can not use --ignoreBlanks when input type is JSON")
		}
	}

	if mongoImport.IngestOptions.UpsertFields != "" {
		mongoImport.IngestOptions.Upsert = true
		mongoImport.upsertFields = strings.Split(mongoImport.IngestOptions.UpsertFields, ",")
		if err := validateFields(mongoImport.upsertFields); err != nil {
			return fmt.Errorf("invalid --upsertFields argument: %v", err)
		}
	} else if mongoImport.IngestOptions.Upsert {
		mongoImport.upsertFields = []string{"_id"}
	}

	if mongoImport.IngestOptions.Upsert {
		mongoImport.IngestOptions.MaintainInsertionOrder = true
		log.Logf(log.Info, "using upsert fields: %v", mongoImport.upsertFields)
	}

	// set the number of decoding workers to use for imports
	if mongoImport.ToolOptions.NumDecodingWorkers <= 0 {
		mongoImport.ToolOptions.NumDecodingWorkers = mongoImport.ToolOptions.MaxProcs
	}
	log.Logf(log.DebugLow, "using %v decoding workers", mongoImport.ToolOptions.NumDecodingWorkers)

	// set the number of insertion workers to use for imports
	if mongoImport.ToolOptions.NumInsertionWorkers <= 0 {
		mongoImport.ToolOptions.NumInsertionWorkers = 1
	}

	log.Logf(log.DebugLow, "using %v insert workers", mongoImport.ToolOptions.NumInsertionWorkers)

	// if --maintainInsertionOrder is set, we can only allow 1 insertion worker
	if mongoImport.IngestOptions.MaintainInsertionOrder {
		mongoImport.ToolOptions.NumInsertionWorkers = 1
	}

	// get the number of documents per batch
	if mongoImport.ToolOptions.BulkBufferSize <= 0 {
		mongoImport.ToolOptions.BulkBufferSize = 10000
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

	if mongoImport.InputOptions.File == "" {
		if len(args) != 0 {
			// if --file is not supplied, use the positional argument supplied
			mongoImport.InputOptions.File = args[0]
		}
	}

	// ensure we have a valid string to use for the collection
	if mongoImport.ToolOptions.Collection == "" {
		log.Logf(log.Always, "no collection specified")
		fileBaseName := filepath.Base(mongoImport.InputOptions.File)
		lastDotIndex := strings.LastIndex(fileBaseName, ".")
		if lastDotIndex != -1 {
			fileBaseName = fileBaseName[0:lastDotIndex]
		}
		log.Logf(log.Always, "using filename '%v' as collection", fileBaseName)
		mongoImport.ToolOptions.Collection = fileBaseName
	}
	err = util.ValidateCollectionName(mongoImport.ToolOptions.Collection)
	if err != nil {
		return fmt.Errorf("invalid collection name: %v", err)
	}
	return nil
}

// getSourceReader returns an io.Reader to read from the input source. Also
// returns a progress.Progressor which can be used to track progress if the
// reader supports it.
func (mongoImport *MongoImport) getSourceReader() (io.ReadCloser, int64, error) {
	if mongoImport.InputOptions.File != "" {
		file, err := os.Open(util.ToUniversalPath(mongoImport.InputOptions.File))
		if err != nil {
			return nil, -1, err
		}
		fileStat, err := file.Stat()
		if err != nil {
			return nil, -1, err
		}
		log.Logf(log.Info, "filesize: %v bytes", fileStat.Size())
		return file, int64(fileStat.Size()), err
	}

	log.Logf(log.Info, "reading from stdin")

	// Stdin has undefined max size, so no progressor
	return os.Stdin, -1, nil
}

// fileSizeProgressor implements Progressor to allow a sizeTracker to hook up with a
// progress.Bar instance, so that the progress bar can report the percentage of the file read.
type fileSizeProgressor struct {
	max int64
	sizeTracker
}

func (fsp *fileSizeProgressor) Progress() (int64, int64) {
	return fsp.max, fsp.sizeTracker.Size()
}

// ImportDocuments is used to write input data to the database. It returns the
// number of documents successfully imported to the appropriate namespace and
// any error encountered in doing this
func (mongoImport *MongoImport) ImportDocuments() (uint64, error) {
	source, fileSize, err := mongoImport.getSourceReader()
	if err != nil {
		return 0, err
	}
	defer source.Close()

	inputReader, err := mongoImport.getInputReader(source)
	if err != nil {
		return 0, err
	}

	if mongoImport.InputOptions.HeaderLine {
		if err = inputReader.ReadAndValidateHeader(); err != nil {
			return 0, err
		}
	}

	if fileSize > 0 {
		bar := &progress.Bar{
			Name:     fmt.Sprintf("%v.%v", mongoImport.ToolOptions.DB, mongoImport.ToolOptions.Collection),
			Watching: &fileSizeProgressor{fileSize, inputReader},
			Writer:   log.Writer(0),
		}
		bar.Start()
	}

	return mongoImport.importDocuments(inputReader)
}

// importDocuments is a helper to ImportDocuments and does all the ingestion
// work by taking data from the inputReader source and writing it to the
// appropriate namespace
func (mongoImport *MongoImport) importDocuments(inputReader InputReader) (numImported uint64, retErr error) {
	session, err := mongoImport.SessionProvider.GetSession()
	if err != nil {
		return 0, err
	}
	defer session.Close()

	connURL := mongoImport.ToolOptions.Host
	if connURL == "" {
		connURL = util.DefaultHost
	}
	if mongoImport.ToolOptions.Port != "" {
		connURL = connURL + ":" + mongoImport.ToolOptions.Port
	}
	log.Logf(log.Always, "connected to: %v", connURL)

	log.Logf(log.Info, "ns: %v.%v",
		mongoImport.ToolOptions.Namespace.DB,
		mongoImport.ToolOptions.Namespace.Collection)

	// check if the server is a replica set
	mongoImport.isReplicaSet, err = mongoImport.SessionProvider.IsReplicaSet()
	if err != nil {
		return 0, fmt.Errorf("error checking if server is part of a replicaset: %v", err)
	}
	log.Logf(log.Info, "is replica set: %v", mongoImport.isReplicaSet)

	if err = mongoImport.configureSession(session); err != nil {
		return 0, fmt.Errorf("error configuring session: %v", err)
	}

	// drop the database if necessary
	if mongoImport.IngestOptions.Drop {
		log.Logf(log.Always, "dropping: %v.%v",
			mongoImport.ToolOptions.DB,
			mongoImport.ToolOptions.Collection)
		collection := session.DB(mongoImport.ToolOptions.DB).
			C(mongoImport.ToolOptions.Collection)
		if err := collection.DropCollection(); err != nil {
			if err.Error() != db.ErrNsNotFound.Error() {
				return 0, err
			}
		}
	}

	readDocChan := make(chan bson.D, workerBufferSize)

	// any read errors should cause mongoimport to stop ingestion immediately
	readErrChan := make(chan error, 1)

	// whether or not documents should be streamed in read order
	ordered := mongoImport.IngestOptions.MaintainInsertionOrder

	// handle all input reads in a separate goroutine
	go inputReader.StreamDocument(ordered, readDocChan, readErrChan)

	// return immediately on ingest errors - these will be triggered
	// either by an issue ingesting data or if the read channel is
	// closed so we can block here while reads happen in a goroutine
	if err = mongoImport.IngestDocuments(readDocChan); err != nil {
		return mongoImport.insertionCount, err
	}
	return mongoImport.insertionCount, <-readErrChan
}

// IngestDocuments takes a slice of documents and either inserts/upserts them -
// based on whether an upsert is requested - into the given collection
func (mongoImport *MongoImport) IngestDocuments(readDocChan chan bson.D) (retErr error) {
	numInsertionWorkers := mongoImport.ToolOptions.NumInsertionWorkers
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

	wg := &sync.WaitGroup{}
	for i := 0; i < numInsertionWorkers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			// only set the first insertion error and cause sibling goroutines to terminate immediately
			if err := mongoImport.runInsertionWorker(readDocChan); err != nil && retErr == nil {
				retErr = err
				mongoImport.Kill(err)
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
//
// returns an error if it's unable to set the write concern
func (mongoImport *MongoImport) configureSession(session *mgo.Session) error {
	// sockets to the database will never be forcibly closed
	session.SetSocketTimeout(0)
	sessionSafety, err := db.BuildWriteConcern(
		mongoImport.IngestOptions.WriteConcern,
		mongoImport.isReplicaSet,
	)
	if err != nil {
		return fmt.Errorf("write concern error: %v", err)
	}
	session.SetSafe(sessionSafety)
	return nil
}

// runInsertionWorker is a helper to InsertDocuments - it reads document off
// the read channel and prepares then in batches for insertion into the databas
func (mongoImport *MongoImport) runInsertionWorker(readDocChan chan bson.D) (err error) {
	session, err := mongoImport.SessionProvider.GetSession()
	if err != nil {
		return fmt.Errorf("error connecting to mongod: %v", err)
	}
	defer session.Close()
	if err = mongoImport.configureSession(session); err != nil {
		return fmt.Errorf("error configuring session: %v", err)
	}
	collection := session.DB(mongoImport.ToolOptions.DB).C(mongoImport.ToolOptions.Collection)
	ignoreBlanks := mongoImport.IngestOptions.IgnoreBlanks && mongoImport.InputOptions.Type != JSON
	documentBytes := make([]byte, 0)
	documents := make([]bson.Raw, 0)
	numMessageBytes := 0

readLoop:
	for {
		select {
		case document, alive := <-readDocChan:
			if !alive {
				break readLoop
			}
			// the mgo driver doesn't currently respect the maxBatchSize
			// limit so we self impose a limit by using maxMessageSizeBytes
			// and send documents over the wire when we hit the batch size
			// or when we're at/over the maximum message size threshold
			if len(documents) == mongoImport.ToolOptions.BulkBufferSize || numMessageBytes >= maxMessageSizeBytes {
				if err = mongoImport.insert(documents, collection); err != nil {
					return err
				}
				// TODO: TOOLS-313; better to use a progress bar here
				if mongoImport.insertionCount%10000 == 0 {
					log.Logf(log.Info, "Progress: %v documents inserted...", mongoImport.insertionCount)
				}
				documents = documents[:0]
				numMessageBytes = 0
			}
			// ignore blank fields if specified
			if ignoreBlanks {
				document = removeBlankFields(document)
			}
			if documentBytes, err = bson.Marshal(document); err != nil {
				return err
			}
			numMessageBytes += len(documentBytes)
			documents = append(documents, bson.Raw{3, documentBytes})
		case <-mongoImport.Dying():
			return nil
		}
	}

	// ingest any documents left in slice
	if len(documents) != 0 {
		return mongoImport.insert(documents, collection)
	}
	return nil
}

// TODO: TOOLS-317: add tests/update this to be more efficient
// handleUpsert upserts documents into the database - used if --upsert is passed
// to mongoimport
func (mongoImport *MongoImport) handleUpsert(documents []bson.Raw, collection *mgo.Collection) (numInserted int, err error) {
	stopOnError := mongoImport.IngestOptions.StopOnError
	for _, rawBsonDocument := range documents {
		document := bson.M{}
		err = bson.Unmarshal(rawBsonDocument.Data, &document)
		if err != nil {
			return numInserted, fmt.Errorf("error unmarshaling document: %v", err)
		}
		selector := constructUpsertDocument(mongoImport.upsertFields, document)
		if selector == nil {
			err = collection.Insert(document)
		} else {
			_, err = collection.Upsert(selector, document)
		}
		if err == nil {
			numInserted += 1
		}
		if err = filterIngestError(stopOnError, err); err != nil {
			return numInserted, err
		}
	}
	return numInserted, nil
}

// insert  performs the actual insertion/updates. If no upsert fields are
// present in the document to be inserted, it simply inserts the documents
// into the given collection
func (mongoImport *MongoImport) insert(documents []bson.Raw, collection *mgo.Collection) (err error) {
	numInserted := 0
	stopOnError := mongoImport.IngestOptions.StopOnError
	maintainInsertionOrder := mongoImport.IngestOptions.MaintainInsertionOrder

	defer func() {
		mongoImport.insertionLock.Lock()
		mongoImport.insertionCount += uint64(numInserted)
		mongoImport.insertionLock.Unlock()
	}()

	if mongoImport.IngestOptions.Upsert {
		numInserted, err = mongoImport.handleUpsert(documents, collection)
		return err
	} else {
		if len(documents) == 0 {
			return
		}
		bulk := collection.Bulk()
		for _, document := range documents {
			bulk.Insert(document)
		}
		if !maintainInsertionOrder {
			bulk.Unordered()
		}

		// mgo.Bulk doesn't currently implement write commands so mgo.BulkResult
		// isn't informative
		_, err = bulk.Run()

		// TOOLS-349: Note that this count may not be entirely accurate if some
		// ingester workers insert when another errors out.
		//
		// Without write commands, we can't say for sure how many documents
		// were inserted when we use bulk inserts so we assume the entire batch
		// insert failed if an error is returned. The result is that we may
		// report that less documents - than were actually inserted - were
		// inserted into the database. This will change as soon as BulkResults
		// are supported by the driver
		if err == nil {
			numInserted = len(documents)
		}
	}
	return filterIngestError(stopOnError, err)
}

// getInputReader returns an implementation of InputReader based on the input type
func (mongoImport *MongoImport) getInputReader(in io.Reader) (InputReader, error) {
	var fields []string
	var err error
	if mongoImport.InputOptions.Fields != nil {
		fields = strings.Split(*mongoImport.InputOptions.Fields, ",")
	} else if mongoImport.InputOptions.FieldFile != nil {
		fields, err = util.GetFieldsFromFile(*mongoImport.InputOptions.FieldFile)
		if err != nil {
			return nil, err
		}
	}

	// header fields validation can only happen once we have an input reader
	if !mongoImport.InputOptions.HeaderLine {
		if err = validateReaderFields(fields); err != nil {
			return nil, err
		}
	}

	if mongoImport.InputOptions.Type == CSV {
		return NewCSVInputReader(fields, in, mongoImport.ToolOptions.NumDecodingWorkers), nil
	} else if mongoImport.InputOptions.Type == TSV {
		return NewTSVInputReader(fields, in, mongoImport.ToolOptions.NumDecodingWorkers), nil
	}
	return NewJSONInputReader(mongoImport.InputOptions.JSONArray, in, mongoImport.ToolOptions.NumDecodingWorkers), nil
}
