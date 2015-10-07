// Package mongoimport allows importing content from a JSON, CSV, or TSV into a MongoDB instance.
package mongoimport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/text"
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

// Input format types accepted by mongoimport.
const (
	CSV  = "csv"
	TSV  = "tsv"
	JSON = "json"
)

const (
	maxBSONSize         = 16 * (1024 * 1024)
	maxMessageSizeBytes = 2 * maxBSONSize
	workerBufferSize    = 16
	progressBarLength   = 24
)

// MongoImport is a container for the user-specified options and
// internal state used for running mongoimport.
type MongoImport struct {
	// generic mongo tool options
	ToolOptions *options.ToolOptions

	// InputOptions defines options used to read data to be ingested
	InputOptions *InputOptions

	// IngestOptions defines options used to ingest data into MongoDB
	IngestOptions *IngestOptions

	// SessionProvider is used for connecting to the database
	SessionProvider *db.SessionProvider

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
	}

	if imp.IngestOptions.UpsertFields != "" {
		imp.IngestOptions.Upsert = true
		imp.upsertFields = strings.Split(imp.IngestOptions.UpsertFields, ",")
		if err := validateFields(imp.upsertFields); err != nil {
			return fmt.Errorf("invalid --upsertFields argument: %v", err)
		}
	} else if imp.IngestOptions.Upsert {
		imp.upsertFields = []string{"_id"}
	}

	if imp.IngestOptions.Upsert {
		imp.IngestOptions.MaintainInsertionOrder = true
		log.Logf(log.Info, "using upsert fields: %v", imp.upsertFields)
	}

	// set the number of decoding workers to use for imports
	if imp.ToolOptions.NumDecodingWorkers <= 0 {
		imp.ToolOptions.NumDecodingWorkers = imp.ToolOptions.MaxProcs
	}
	log.Logf(log.DebugLow, "using %v decoding workers", imp.ToolOptions.NumDecodingWorkers)

	// set the number of insertion workers to use for imports
	if imp.IngestOptions.NumInsertionWorkers <= 0 {
		imp.IngestOptions.NumInsertionWorkers = 1
	}

	log.Logf(log.DebugLow, "using %v insert workers", imp.IngestOptions.NumInsertionWorkers)

	// if --maintainInsertionOrder is set, we can only allow 1 insertion worker
	if imp.IngestOptions.MaintainInsertionOrder {
		imp.IngestOptions.NumInsertionWorkers = 1
	}

	// get the number of documents per batch
	if imp.ToolOptions.BulkBufferSize <= 0 {
		imp.ToolOptions.BulkBufferSize = 10000
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
		log.Logf(log.Always, "no collection specified")
		fileBaseName := filepath.Base(imp.InputOptions.File)
		lastDotIndex := strings.LastIndex(fileBaseName, ".")
		if lastDotIndex != -1 {
			fileBaseName = fileBaseName[0:lastDotIndex]
		}
		log.Logf(log.Always, "using filename '%v' as collection", fileBaseName)
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
		log.Logf(log.Info, "filesize: %v bytes", fileStat.Size())
		return file, int64(fileStat.Size()), err
	}

	log.Logf(log.Info, "reading from stdin")

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
	return fsp.max, fsp.sizeTracker.Size()
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
		if err = inputReader.ReadAndValidateHeader(); err != nil {
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
	log.Logf(log.Always, "connected to: %v", connURL)

	log.Logf(log.Info, "ns: %v.%v",
		imp.ToolOptions.Namespace.DB,
		imp.ToolOptions.Namespace.Collection)

	// check if the server is a replica set, mongos, or standalone
	imp.nodeType, err = imp.SessionProvider.GetNodeType()
	if err != nil {
		return 0, fmt.Errorf("error checking connected node type: %v", err)
	}
	log.Logf(log.Info, "connected to node type: %v", imp.nodeType)

	if err = imp.configureSession(session); err != nil {
		return 0, fmt.Errorf("error configuring session: %v", err)
	}

	// drop the database if necessary
	if imp.IngestOptions.Drop {
		log.Logf(log.Always, "dropping: %v.%v",
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

	return imp.insertionCount, channelQuorumError(processingErrChan, 2)
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

	wg := &sync.WaitGroup{}
	mt := &sync.Mutex{}
	for i := 0; i < numInsertionWorkers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			// only set the first insertion error and cause sibling goroutines to terminate immediately
			err := imp.runInsertionWorker(readDocs)
			mt.Lock()
			defer mt.Unlock()
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
//
// returns an error if it's unable to set the write concern
func (imp *MongoImport) configureSession(session *mgo.Session) error {
	// sockets to the database will never be forcibly closed
	session.SetSocketTimeout(0)
	sessionSafety, err := db.BuildWriteConcern(imp.IngestOptions.WriteConcern, imp.nodeType)
	if err != nil {
		return fmt.Errorf("write concern error: %v", err)
	}
	session.SetSafe(sessionSafety)
	return nil
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
	ignoreBlanks := imp.IngestOptions.IgnoreBlanks && imp.InputOptions.Type != JSON
	var documentBytes []byte
	var documents []bson.Raw
	numMessageBytes := 0

readLoop:
	for {
		select {
		case document, alive := <-readDocs:
			if !alive {
				break readLoop
			}
			// the mgo driver doesn't currently respect the maxBatchSize
			// limit so we self impose a limit by using maxMessageSizeBytes
			// and send documents over the wire when we hit the batch size
			// or when we're at/over the maximum message size threshold
			if len(documents) == imp.ToolOptions.BulkBufferSize || numMessageBytes >= maxMessageSizeBytes {
				if err = imp.insert(documents, collection); err != nil {
					return err
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
			if len(documentBytes) > maxBSONSize {
				log.Logf(log.Always, "warning: attempting to insert document with size %v (exceeds %v limit)",
					text.FormatByteAmount(int64(len(documentBytes))), text.FormatByteAmount(maxBSONSize))
			}
			numMessageBytes += len(documentBytes)
			documents = append(documents, bson.Raw{3, documentBytes})
		case <-imp.Dying():
			return nil
		}
	}

	// ingest any documents left in slice
	if len(documents) != 0 {
		return imp.insert(documents, collection)
	}
	return nil
}

// TODO: TOOLS-317: add tests/update this to be more efficient
// handleUpsert upserts documents into the database - used if --upsert is passed
// to mongoimport
func (imp *MongoImport) handleUpsert(documents []bson.Raw, collection *mgo.Collection) (numInserted int, err error) {
	stopOnError := imp.IngestOptions.StopOnError
	for _, rawBsonDocument := range documents {
		document := bson.D{}
		err = bson.Unmarshal(rawBsonDocument.Data, &document)
		if err != nil {
			return numInserted, fmt.Errorf("error unmarshaling document: %v", err)
		}
		selector := constructUpsertDocument(imp.upsertFields, document)
		if selector == nil {
			err = collection.Insert(document)
		} else {
			_, err = collection.Upsert(selector, document)
		}
		if err == nil {
			numInserted++
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
func (imp *MongoImport) insert(documents []bson.Raw, collection *mgo.Collection) (err error) {
	numInserted := 0
	stopOnError := imp.IngestOptions.StopOnError
	maintainInsertionOrder := imp.IngestOptions.MaintainInsertionOrder

	defer func() {
		imp.insertionLock.Lock()
		imp.insertionCount += uint64(numInserted)
		imp.insertionLock.Unlock()
	}()

	if imp.IngestOptions.Upsert {
		numInserted, err = imp.handleUpsert(documents, collection)
		return err
	}
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
	return filterIngestError(stopOnError, err)
}

// getInputReader returns an implementation of InputReader based on the input type
func (imp *MongoImport) getInputReader(in io.Reader) (InputReader, error) {
	var fields []string
	var err error
	if imp.InputOptions.Fields != nil {
		fields = strings.Split(*imp.InputOptions.Fields, ",")
	} else if imp.InputOptions.FieldFile != nil {
		fields, err = util.GetFieldsFromFile(*imp.InputOptions.FieldFile)
		if err != nil {
			return nil, err
		}
	}

	// header fields validation can only happen once we have an input reader
	if !imp.InputOptions.HeaderLine {
		if err = validateReaderFields(fields); err != nil {
			return nil, err
		}
	}

	if imp.InputOptions.Type == CSV {
		return NewCSVInputReader(fields, in, imp.ToolOptions.NumDecodingWorkers), nil
	} else if imp.InputOptions.Type == TSV {
		return NewTSVInputReader(fields, in, imp.ToolOptions.NumDecodingWorkers), nil
	}
	return NewJSONInputReader(imp.InputOptions.JSONArray, in, imp.ToolOptions.NumDecodingWorkers), nil
}
