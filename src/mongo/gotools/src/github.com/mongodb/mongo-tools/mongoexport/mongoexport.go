// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package mongoexport produces a JSON or CSV export of data stored in a MongoDB instance.
package mongoexport

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/mongodb/mongo-tools-common/bsonutil"
	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools-common/json"
	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/options"
	"github.com/mongodb/mongo-tools-common/progress"
	"github.com/mongodb/mongo-tools-common/util"
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo"
	driverOpts "go.mongodb.org/mongo-driver/mongo/options"
	"go.mongodb.org/mongo-driver/mongo/readpref"
)

// Output types supported by mongoexport.
const (
	CSV                            = "csv"
	JSON                           = "json"
	watchProgressorUpdateFrequency = 8000
)

// jsonFormat is the type for all valid extended JSON formats to output.
type jsonFormat string

const (
	canonical jsonFormat = "canonical"
	relaxed              = "relaxed"
)

const (
	progressBarLength   = 24
	progressBarWaitTime = time.Second
)

// MongoExport is a container for the user-specified options and
// internal state used for running mongoexport.
type MongoExport struct {
	// generic mongo tool options
	ToolOptions *options.ToolOptions

	// OutputOpts controls options for how the exported data should be formatted
	OutputOpts *OutputFormatOptions

	InputOpts *InputOptions

	// for connecting to the db
	SessionProvider *db.SessionProvider
	ExportOutput    ExportOutput

	ProgressManager progress.Manager

	// Cached version of the collection info
	collInfo *db.CollectionInfo
}

// ExportOutput is an interface that specifies how a document should be formatted
// and written to an output stream.
type ExportOutput interface {
	// WriteHeader outputs any pre-record headers that are written once
	// per output file.
	WriteHeader() error

	// WriteRecord writes the given document to the given io.Writer according to
	// the format supported by the underlying ExportOutput implementation.
	ExportDocument(bson.D) error

	// WriteFooter outputs any post-record headers that are written once per
	// output file.
	WriteFooter() error

	// Flush writes any pending data to the underlying I/O stream.
	Flush() error
}

// New constructs a new MongoExport instance from the provided options.
func New(opts Options) (*MongoExport, error) {
	exporter := &MongoExport{
		ToolOptions: opts.ToolOptions,
		OutputOpts:  opts.OutputFormatOptions,
		InputOpts:   opts.InputOptions,
	}

	err := exporter.validateSettings()
	if err != nil {
		return nil, util.SetupError{
			Err:     err,
			Message: util.ShortUsage("mongoexport"),
		}
	}

	provider, err := db.NewSessionProvider(*opts.ToolOptions)
	if err != nil {
		return nil, util.SetupError{Err: err}
	}

	log.Logvf(log.Always, "connected to: %v", util.SanitizeURI(opts.URI.ConnectionString))

	isMongos, err := provider.IsMongos()
	if err != nil {
		provider.Close()
		return nil, util.SetupError{Err: err}
	}

	// warn if we are trying to export from a secondary in a sharded cluster
	pref := opts.ToolOptions.ReadPreference
	if isMongos && pref != nil && pref.Mode() != readpref.PrimaryMode {
		log.Logvf(log.Always, db.WarningNonPrimaryMongosConnection)
	}

	progressManager := progress.NewBarWriter(log.Writer(0), progressBarWaitTime, progressBarLength, false)
	progressManager.Start()

	exporter.SessionProvider = provider
	exporter.ProgressManager = progressManager
	return exporter, nil
}

// Close cleans up all the resources for a MongoExport instance.
func (exp *MongoExport) Close() {
	exp.SessionProvider.Close()
	if barWriter, ok := exp.ProgressManager.(*progress.BarWriter); ok {
		barWriter.Stop()
	}
}

// validateSettings returns an error if any settings specified on the command line
// were invalid, or nil if they are valid.
func (exp *MongoExport) validateSettings() error {
	// Namespace must have a valid database if none is specified,
	// use 'test'
	if exp.ToolOptions.Namespace.DB == "" {
		exp.ToolOptions.Namespace.DB = "test"
	}
	err := util.ValidateDBName(exp.ToolOptions.Namespace.DB)
	if err != nil {
		return err
	}

	if exp.ToolOptions.Namespace.Collection == "" {
		return fmt.Errorf("must specify a collection")
	}
	if err = util.ValidateCollectionGrammar(exp.ToolOptions.Namespace.Collection); err != nil {
		return err
	}

	exp.OutputOpts.Type = strings.ToLower(exp.OutputOpts.Type)

	if exp.OutputOpts.CSVOutputType {
		log.Logv(log.Always, "csv flag is deprecated; please use --type=csv instead")
		exp.OutputOpts.Type = CSV
	}

	if exp.OutputOpts.Type == "" {
		// special error for an empty type value
		return fmt.Errorf("--type cannot be empty")
	}
	if exp.OutputOpts.Type != CSV && exp.OutputOpts.Type != JSON {
		return fmt.Errorf("invalid output type '%v', choose 'json' or 'csv'", exp.OutputOpts.Type)
	}

	if exp.OutputOpts.JSONFormat != canonical && exp.OutputOpts.JSONFormat != relaxed {
		return fmt.Errorf("invalid JSON format '%v', choose 'relaxed' or 'canonical'", exp.OutputOpts.JSONFormat)
	}

	if exp.InputOpts.Query != "" && exp.InputOpts.ForceTableScan {
		return fmt.Errorf("cannot use --forceTableScan when specifying --query")
	}

	if exp.InputOpts.Query != "" && exp.InputOpts.QueryFile != "" {
		return fmt.Errorf("either --query or --queryFile can be specified as a query option")
	}

	if exp.InputOpts != nil && exp.InputOpts.HasQuery() {
		content, err := exp.InputOpts.GetQuery()
		if err != nil {
			return err
		}
		_, err2 := getObjectFromByteArg(content)
		if err2 != nil {
			return err2
		}
	}

	if exp.InputOpts != nil && exp.InputOpts.Sort != "" {
		_, err := getSortFromArg(exp.InputOpts.Sort)
		if err != nil {
			return err
		}
	}
	return nil
}

// GetOutputWriter opens and returns an io.WriteCloser for the output
// options or nil if none is set. The caller is responsible for closing it.
func (exp *MongoExport) GetOutputWriter() (io.WriteCloser, error) {
	if exp.OutputOpts.OutputFile != "" {
		// If the directory in which the output file is to be
		// written does not exist, create it
		fileDir := filepath.Dir(exp.OutputOpts.OutputFile)
		err := os.MkdirAll(fileDir, 0750)
		if err != nil {
			return nil, err
		}

		file, err := os.Create(util.ToUniversalPath(exp.OutputOpts.OutputFile))
		if err != nil {
			return nil, err
		}
		return file, err
	}
	// No writer, so caller should assume Stdout (or some other reasonable default)
	return nil, nil
}

// Take a comma-delimited set of field names and build a selector doc for query projection.
// For fields containing a dot '.', we project the entire top-level portion.
// e.g. "a,b,c.d.e,f.$" -> {a:1, b:1, "c":1, "f.$": 1}.
func makeFieldSelector(fields string) bson.M {
	selector := bson.M{"_id": 1}
	if fields == "" {
		return selector
	}

	for _, field := range strings.Split(fields, ",") {
		// Projections like "a.0" work fine for nested documents not for arrays
		// - if passed directly to mongod. To handle this, we have to retrieve
		// the entire top-level document and then filter afterwards. An exception
		// is made for '$' projections - which are passed directly to mongod.
		if i := strings.LastIndex(field, "."); i != -1 && field[i+1:] != "$" {
			field = field[:strings.Index(field, ".")]
		}
		selector[field] = 1
	}
	return selector
}

// getCount returns an estimate of how many documents the cursor will fetch
// It always returns Limit if there is a limit, assuming that in general
// limits will less then the total possible.
// If there is a query and no limit then it returns 0, because it's too expensive to count the query.
// If the collection is a view then it returns 0, because it is too expensive to count the view.
// Otherwise it returns the count minus the skip
func (exp *MongoExport) getCount() (int64, error) {
	session, err := exp.SessionProvider.GetSession()
	if err != nil {
		return 0, err
	}
	if exp.InputOpts != nil && exp.InputOpts.Limit != 0 {
		return exp.InputOpts.Limit, nil
	}
	if exp.InputOpts != nil && exp.InputOpts.Query != "" {
		return 0, nil
	}
	coll := session.Database(exp.ToolOptions.Namespace.DB).Collection(exp.ToolOptions.Namespace.Collection)

	if exp.collInfo.IsView() {
		return 0, nil
	}

	c, err := coll.EstimatedDocumentCount(nil)
	if err != nil {
		return 0, err
	}

	var skip int64
	if exp.InputOpts != nil {
		skip = exp.InputOpts.Skip
	}
	if skip > c {
		c = 0
	} else {
		c -= skip
	}
	return c, nil
}

// getCursor returns a cursor that can be iterated over to get all the documents
// to export, based on the options given to mongoexport. Also returns the
// associated session, so that it can be closed once the cursor is used up.
func (exp *MongoExport) getCursor() (*mongo.Cursor, error) {
	findOpts := driverOpts.Find()

	if exp.InputOpts != nil && exp.InputOpts.Sort != "" {
		sortD, err := getSortFromArg(exp.InputOpts.Sort)
		if err != nil {
			return nil, err
		}

		findOpts.SetSort(sortD)
	}

	query := bson.D{}
	if exp.InputOpts != nil && exp.InputOpts.HasQuery() {
		var err error
		content, err := exp.InputOpts.GetQuery()
		if err != nil {
			return nil, err
		}
		err = bson.UnmarshalExtJSON(content, false, &query)
		if err != nil {
			return nil, fmt.Errorf("error parsing query as Extended JSON: %v", err)
		}
	}

	session, err := exp.SessionProvider.GetSession()
	if err != nil {
		return nil, err
	}
	coll := session.Database(exp.ToolOptions.Namespace.DB).Collection(exp.ToolOptions.Namespace.Collection)

	// don't snapshot if we've been asked not to,
	// or if we cannot because  we are querying, sorting, or if the collection is a view
	if !exp.InputOpts.ForceTableScan && len(query) == 0 && exp.InputOpts != nil && exp.InputOpts.Sort == "" &&
		!exp.collInfo.IsView() && !exp.collInfo.IsSystemCollection() {

		// Don't hint autoIndexId:false collections
		autoIndexId, found := exp.collInfo.Options["autoIndexId"]
		if !found || autoIndexId == true {
			findOpts.SetHint(bson.D{{"_id", 1}})
		}
	}

	if exp.InputOpts != nil {
		findOpts.SetSkip(exp.InputOpts.Skip)
	}
	if exp.InputOpts != nil {
		findOpts.SetLimit(exp.InputOpts.Limit)
	}

	if len(exp.OutputOpts.Fields) > 0 {
		findOpts.SetProjection(makeFieldSelector(exp.OutputOpts.Fields))
	}

	return coll.Find(nil, query, findOpts)
}

// verifyCollectionExists checks if the collection exists. If it does, a copy of the collection info will be cached
// on the receiver. If the collection does not exist and AssertExists was specified, a non-nil error is returned.
func (exp *MongoExport) verifyCollectionExists() (bool, error) {
	session, err := exp.SessionProvider.GetSession()
	if err != nil {
		return false, err
	}

	coll := session.Database(exp.ToolOptions.Namespace.DB).Collection(exp.ToolOptions.Namespace.Collection)
	exp.collInfo, err = db.GetCollectionInfo(coll)
	if err != nil {
		return false, err
	}

	// If the collection doesn't exist, GetCollectionInfo will return nil
	if exp.collInfo == nil {
		var collInfoErr error
		if exp.InputOpts.AssertExists {
			collInfoErr = fmt.Errorf("collection '%s' does not exist", exp.ToolOptions.Namespace.Collection)
		}

		return false, collInfoErr
	}

	return true, nil
}

// Internal function that handles exporting to the given writer. Used primarily
// for testing, because it bypasses writing to the file system.
func (exp *MongoExport) exportInternal(out io.Writer) (int64, error) {
	// Check if the collection exists before starting export
	exists, err := exp.verifyCollectionExists()
	if err != nil || !exists {
		return 0, err
	}

	max, err := exp.getCount()
	if err != nil {
		return 0, err
	}

	watchProgressor := progress.NewCounter(int64(max))
	if exp.ProgressManager != nil {
		name := fmt.Sprintf("%v.%v", exp.ToolOptions.Namespace.DB, exp.ToolOptions.Namespace.Collection)
		exp.ProgressManager.Attach(name, watchProgressor)
		defer exp.ProgressManager.Detach(name)
	}

	exportOutput, err := exp.getExportOutput(out)
	if err != nil {
		return 0, err
	}

	cursor, err := exp.getCursor()
	if err != nil {
		return 0, err
	}
	defer cursor.Close(nil)

	// Write headers
	err = exportOutput.WriteHeader()
	if err != nil {
		return 0, err
	}

	docsCount := int64(0)

	// Write document content
	for cursor.Next(nil) {
		var result bson.D
		if err := cursor.Decode(&result); err != nil {
			return docsCount, err
		}

		err := exportOutput.ExportDocument(result)
		if err != nil {
			return docsCount, err
		}
		docsCount++
		if docsCount%watchProgressorUpdateFrequency == 0 {
			watchProgressor.Set(docsCount)
		}
	}
	watchProgressor.Set(docsCount)
	if err := cursor.Err(); err != nil {
		return docsCount, err
	}

	// Write footers
	err = exportOutput.WriteFooter()
	if err != nil {
		return docsCount, err
	}
	exportOutput.Flush()
	return docsCount, nil
}

// Export executes the entire export operation. It returns an integer of the count
// of documents successfully exported, and a non-nil error if something went wrong
// during the export operation.
func (exp *MongoExport) Export(out io.Writer) (int64, error) {
	count, err := exp.exportInternal(out)
	return count, err
}

// getExportOutput returns an implementation of ExportOutput which can handle
// transforming BSON documents into the appropriate output format and writing
// them to an output stream.
func (exp *MongoExport) getExportOutput(out io.Writer) (ExportOutput, error) {
	if exp.OutputOpts.Type == CSV {
		// TODO what if user specifies *both* --fields and --fieldFile?
		var fields []string
		var err error
		if len(exp.OutputOpts.Fields) > 0 {
			fields = strings.Split(exp.OutputOpts.Fields, ",")
		} else if exp.OutputOpts.FieldFile != "" {
			fields, err = util.GetFieldsFromFile(exp.OutputOpts.FieldFile)
			if err != nil {
				return nil, err
			}
		} else {
			return nil, fmt.Errorf("CSV mode requires a field list")
		}

		exportFields := make([]string, 0, len(fields))
		for _, field := range fields {
			// for '$' field projections, exclude '.$' from the field name
			if i := strings.LastIndex(field, "."); i != -1 && field[i+1:] == "$" {
				exportFields = append(exportFields, field[:i])
			} else {
				exportFields = append(exportFields, field)
			}
		}

		return NewCSVExportOutput(exportFields, exp.OutputOpts.NoHeaderLine, out), nil
	}
	return NewJSONExportOutput(exp.OutputOpts.JSONArray, exp.OutputOpts.Pretty, out, exp.OutputOpts.JSONFormat), nil
}

// getObjectFromByteArg takes an object in extended JSON, and converts it to an object that
// can be passed straight to db.collection.find(...) as a query or sort criteria.
// Returns an error if the string is not valid JSON, or extended JSON.
func getObjectFromByteArg(queryRaw []byte) (map[string]interface{}, error) {
	parsedJSON := map[string]interface{}{}
	err := json.Unmarshal(queryRaw, &parsedJSON)
	if err != nil {
		return nil, fmt.Errorf("query '%v' is not valid JSON: %v", queryRaw, err)
	}

	err = bsonutil.ConvertLegacyExtJSONDocumentToBSON(parsedJSON)
	if err != nil {
		return nil, err
	}
	return parsedJSON, nil
}

// getSortFromArg takes a sort specification in JSON and returns it as a bson.D
// object which preserves the ordering of the keys as they appear in the input.
func getSortFromArg(queryRaw string) (bson.D, error) {
	parsedJSON := bson.D{}
	err := json.Unmarshal([]byte(queryRaw), &parsedJSON)
	if err != nil {
		return nil, fmt.Errorf("query '%v' is not valid JSON: %v", queryRaw, err)
	}
	// TODO: verify sort specification before returning a nil error
	return parsedJSON, nil
}
