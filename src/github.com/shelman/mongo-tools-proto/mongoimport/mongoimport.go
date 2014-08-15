package mongoimport

import (
	"errors"
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	commonOpts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/common/util"
	"github.com/shelman/mongo-tools-proto/mongoimport/options"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strings"
)

const (
	CSV  = "csv"
	TSV  = "tsv"
	JSON = "json"
)

// compile-time interface sanity check
var (
	_ ImportInput = (*CSVImportInput)(nil)
	_ ImportInput = (*TSVImportInput)(nil)
	_ ImportInput = (*JSONImportInput)(nil)
)

var (
	errNsNotFound = errors.New("ns not found")
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
}

// ImportInput is an interface that specifies how an input source should be
// converted to BSON
type ImportInput interface {
	// ImportDocument reads the given record from the given io.Reader according
	// to the format supported by the underlying ImportInput implementation.
	ImportDocument() (bson.M, error)

	// SetHeader sets the header for the CSV/TSV import when --headerline is
	// specified
	SetHeader() error
}

// ValidateSettings ensures that the tool specific options supplied for
// MongoImport are valid
func (mongoImport *MongoImport) ValidateSettings() error {
	// Namespace must have a valid database
	if mongoImport.ToolOptions.Namespace.DB == "" {
		return fmt.Errorf("must specify a database")
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
		}
	}

	// ensure we have a valid string to use for the collection
	if mongoImport.ToolOptions.Namespace.Collection == "" {
		if mongoImport.InputOptions.File == "" {
			return fmt.Errorf("must specify a collection or filename")
		}
		fileBaseName := filepath.Base(mongoImport.InputOptions.File)
		lastDotIndex := strings.LastIndex(fileBaseName, ".")
		if lastDotIndex != -1 {
			fileBaseName = fileBaseName[0:lastDotIndex]
		}
		mongoImport.ToolOptions.Namespace.Collection = fileBaseName
		util.PrintlnTimeStamped("no collection specified!")
		util.PrintfTimeStamped("using filename '%v' as collection\n", fileBaseName)
	}
	return nil
}

// getInputReader returns an io.Reader corresponding to the input location
func (mongoImport *MongoImport) getInputReader() (io.ReadCloser, error) {
	if mongoImport.InputOptions.File != "" {
		file, err := os.Open(mongoImport.InputOptions.File)
		if err != nil {
			return nil, err
		}
		return file, err
	}
	return os.Stdin, nil
}

// ImportDocuments is used to write input data to the database. It returns the
// number of documents successfully imported to the appropriate namespace and
// any error encountered in doing this
func (mongoImport *MongoImport) ImportDocuments() (int64, error) {
	in, err := mongoImport.getInputReader()
	if err != nil {
		return 0, err
	}

	defer in.Close()

	importInput, err := mongoImport.getImportInput(in)
	if err != nil {
		return 0, err
	}

	if mongoImport.InputOptions.HeaderLine {
		err = importInput.SetHeader()
		if err != nil {
			return 0, err
		}
	}
	return mongoImport.importDocuments(importInput)
}

// importDocuments is a helper to ImportDocuments and does all the ingestion
// work by taking data from the 'importInput' source and writing it to the
// appropriate namespace
func (mongoImport *MongoImport) importDocuments(importInput ImportInput) (
	int64, error) {
	session := mongoImport.SessionProvider.GetSession()
	defer session.Close()
	connUrl := mongoImport.ToolOptions.Host
	if mongoImport.ToolOptions.Port != "" {
		connUrl = connUrl + ":" + mongoImport.ToolOptions.Port
	}
	fmt.Fprintf(os.Stdout, "connected to: %v\n", connUrl)
	collection := session.DB(mongoImport.ToolOptions.DB).
		C(mongoImport.ToolOptions.Collection)

	// drop the database if necessary
	if mongoImport.IngestOptions.Drop {
		util.PrintfTimeStamped("dropping: %v.%v\n", mongoImport.ToolOptions.DB,
			mongoImport.ToolOptions.Collection)
		if err := collection.DropCollection(); err != nil {
			// this is hacky but necessary :(
			if err.Error() != errNsNotFound.Error() {
				return 0, err
			}
		}
	}

	// trim upsert fields if supplied
	var upsertFields []string
	if mongoImport.IngestOptions.Upsert {
		if len(mongoImport.IngestOptions.UpsertFields) != 0 {
			upsertFields = strings.Split(mongoImport.IngestOptions.UpsertFields,
				",")
		}
	}

	docsCount := int64(0)
	for {
		document, err := importInput.ImportDocument()
		if err != nil {
			if err == io.EOF {
				return docsCount, nil
			}
			if mongoImport.IngestOptions.StopOnError {
				return docsCount, err
			}
			if document == nil {
				return docsCount, err
			}
			continue
		}

		// ignore blank fields if specified
		if mongoImport.IngestOptions.IgnoreBlanks &&
			mongoImport.InputOptions.Type != JSON {
			document = removeBlankFields(document)
		}

		// if upsert is specified without any fields, default to inserts
		if mongoImport.IngestOptions.Upsert {
			selector := constructUpsertDocument(upsertFields, document)
			if selector == nil {
				err = collection.Insert(document)
			} else {
				_, err = collection.Upsert(selector, document)
			}
		} else {
			err = collection.Insert(document)
		}
		if err != nil {
			if mongoImport.IngestOptions.StopOnError {
				return docsCount, err
			}
			fmt.Fprintf(os.Stderr, "error inserting document: %v\n", err)
			continue
		}
		docsCount++
	}
	return docsCount, nil
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

// removeBlankFields removes empty/blank fields in csv and tsv
func removeBlankFields(document bson.M) bson.M {
	for key, value := range document {
		if reflect.TypeOf(value).Kind() == reflect.String &&
			value.(string) == "" {
			delete(document, key)
		}
	}
	return document
}

// getImportInput returns an implementation of ImportInput which can handle
// transforming tsv, csv, or JSON into appropriate BSON documents
func (mongoImport *MongoImport) getImportInput(in io.Reader) (ImportInput,
	error) {
	var fields []string
	var err error
	// there should be some sanity checks done for field names - e.g. that they
	// don't contain dots
	if len(mongoImport.InputOptions.Fields) != 0 {
		fields = strings.Split(strings.Trim(mongoImport.InputOptions.Fields,
			" "), ",")
	} else if mongoImport.InputOptions.FieldFile != "" {
		fields, err = util.GetFieldsFromFile(mongoImport.InputOptions.FieldFile)
		if err != nil {
			return nil, err
		}
	}
	if mongoImport.InputOptions.Type == CSV {
		return NewCSVImportInput(fields, in), nil
	} else if mongoImport.InputOptions.Type == TSV {
		return NewTSVImportInput(fields, in), nil
	}
	return NewJSONImportInput(mongoImport.InputOptions.JSONArray, in), nil
}
