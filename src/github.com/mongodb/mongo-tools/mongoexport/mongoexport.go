package mongoexport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/db"
	sloppyjson "github.com/mongodb/mongo-tools/common/json"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongoexport/options"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"path/filepath"
	"strings"
)

const (
	JSON_ARRAY_START = "["
	JSON_ARRAY_END   = "]"
)

// compile-time interface sanity check
var (
	_ ExportOutput = (*CSVExportOutput)(nil)
	_ ExportOutput = (*JSONExportOutput)(nil)
)

// Wrapper for mongoexport functionality
type MongoExport struct {
	// generic mongo tool options
	ToolOptions commonopts.ToolOptions

	// OutputOpts controls options for how the exported data should be formatted
	OutputOpts *options.OutputFormatOptions

	InputOpts *options.InputOptions

	// for connecting to the db
	cmdRunner       db.CommandRunner
	SessionProvider *db.SessionProvider
	ExportOutput    ExportOutput
}

func (exp *MongoExport) Init() error {
	if exp.ToolOptions.Namespace.DBPath != "" {
		exp.ToolOptions.Namespace.DBPath = util.ToUniversalPath(exp.ToolOptions.Namespace.DBPath)
		shim, err := db.NewShim(exp.ToolOptions.Namespace.DBPath, exp.ToolOptions.DirectoryPerDB, exp.ToolOptions.Journal)
		if err != nil {
			return err
		}
		exp.cmdRunner = shim
		return nil
	}
	exp.cmdRunner = db.NewSessionProvider(exp.ToolOptions)
	return nil
}

// ValidateSettings returns an error if any settings specified on the command line
// were invalid, or nil if they are valid.
func (exp *MongoExport) ValidateSettings() error {
	// TODO - on legacy mongoexport, if -d is blank, it assumes some default database.
	// Do we want to use that same behavior? It seems very odd to assume the DB
	// when only a collection is provided, but that's the behavior of the legacy tools.
	if err := exp.ToolOptions.Validate(); err != nil {
		return err
	}

	// Namespace must have a valid database if none is specified,
	// use 'test'
	if exp.ToolOptions.Namespace.DB == "" {
		exp.ToolOptions.Namespace.DB = "test"
	} else {
		if err := util.ValidateDBName(exp.ToolOptions.Namespace.DB); err != nil {
			return err
		}
	}

	if exp.ToolOptions.Namespace.Collection == "" {
		return fmt.Errorf("must specify a collection")
	} else {
		if err := util.ValidateCollectionName(exp.ToolOptions.Namespace.Collection); err != nil {
			return err
		}
	}

	if exp.InputOpts != nil && exp.InputOpts.Query != "" {
		_, err := getObjectFromArg(exp.InputOpts.Query)
		if err != nil {
			return err
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

// getOutputWriter returns an io.Writer corresponding to the output location
// specified in the options.
func (exp *MongoExport) getOutputWriter() (io.WriteCloser, error) {
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
	return os.Stdout, nil
}

func getDocSource(exp MongoExport) (db.DocSource, error) {
	sortFields := []string{}
	if exp.InputOpts != nil && exp.InputOpts.Sort != "" {
		sortD, err := getSortFromArg(exp.InputOpts.Sort)
		if err != nil {
			return nil, err
		}
		sortFields, err = bsonutil.MakeSortString(sortD)
		if err != nil {
			return nil, err
		}
	}

	query := map[string]interface{}{}
	if exp.InputOpts != nil && exp.InputOpts.Query != "" {
		var err error
		query, err = getObjectFromArg(exp.InputOpts.Query)
		if err != nil {
			return nil, err
		}
	}

	flags := 0
	if len(query) == 0 && exp.InputOpts != nil && exp.InputOpts.ForceTableScan != true && exp.InputOpts.Sort == "" {
		flags = flags | db.Snapshot
	}

	return exp.cmdRunner.FindDocs(
		exp.ToolOptions.Namespace.DB,
		exp.ToolOptions.Namespace.Collection,
		exp.InputOpts.Skip,
		exp.InputOpts.Limit,
		query,
		sortFields,
		flags,
	)
}

// Export executes the entire export operation. It returns an integer of the count
// of documents successfully exported, and a non-nil error if something went wrong
// during the export operation.
func (exp *MongoExport) Export() (int64, error) {
	out, err := exp.getOutputWriter()
	if err != nil {
		return 0, err
	}
	defer out.Close()

	exportOutput, err := exp.getExportOutput(out)
	if err != nil {
		return 0, err
	}

	docSource, err := getDocSource(*exp)
	if err != nil {
		return 0, err
	}

	defer docSource.Close()

	// Write headers
	err = exportOutput.WriteHeader()
	if err != nil {
		return 0, err
	}

	var result bson.M

	docsCount := int64(0)
	// Write document content
	for docSource.Next(&result) {
		err := exportOutput.ExportDocument(result)
		if err != nil {
			return docsCount, err
		}
		docsCount++
	}
	if err := docSource.Err(); err != nil {
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

// getExportOutput returns an implementation of ExportOutput which can handle
// transforming BSON documents into the appropriate output format and writing
// them to an output stream.
func (exp *MongoExport) getExportOutput(out io.Writer) (ExportOutput, error) {
	if exp.OutputOpts.CSV {
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
			return nil, fmt.Errorf("csv mode requires a field list")
		}
		return NewCSVExportOutput(fields, out), nil
	}
	return NewJSONExportOutput(exp.OutputOpts.JSONArray, out), nil
}

// ExportOutput is an interface that specifies how a document should be formatted
// and written to an output stream
type ExportOutput interface {
	// WriteHeader outputs any pre-record headers that are written once
	// per output file.
	WriteHeader() error

	// WriteRecord writes the given document to the given io.Writer according to
	// the format supported by the underlying ExportOutput implementation.
	ExportDocument(bson.M) error

	// WriteFooter outputs any post-record headers that are written once per
	// output file.
	WriteFooter() error

	// Flush writes any pending data to the underlying I/O stream.
	Flush() error
}

// getObjectFromArg takes an object in extended JSON, and converts it to an object that
// can be passed straight to db.collection.find(...) as a query or sort critera.
// Returns an error if the string is not valid JSON, or extended JSON.
func getObjectFromArg(queryRaw string) (map[string]interface{}, error) {
	parsedJSON := map[string]interface{}{}
	err := sloppyjson.Unmarshal([]byte(queryRaw), &parsedJSON)
	if err != nil {
		return nil, fmt.Errorf("Query '%v' is not valid JSON: %v", queryRaw, err)
	}

	for key, val := range parsedJSON {
		if valSubDoc, ok := val.(map[string]interface{}); ok {
			newVal, err := bsonutil.ParseSpecialKeys(valSubDoc)
			if err != nil {
				return nil, fmt.Errorf("Error in query: %v", err)
			}
			parsedJSON[key] = newVal
		}
	}
	return parsedJSON, nil
}

// getSortFromArg takes a sort specification in JSON and returns it as a bson.D
// object which preserves the ordering of the keys as they appear in the input.
func getSortFromArg(queryRaw string) (bson.D, error) {
	parsedJSON := bson.D{}
	err := sloppyjson.Unmarshal([]byte(queryRaw), &parsedJSON)
	if err != nil {
		return nil, fmt.Errorf("Query is not valid JSON: %v", err)
	}
	return parsedJSON, nil
}
