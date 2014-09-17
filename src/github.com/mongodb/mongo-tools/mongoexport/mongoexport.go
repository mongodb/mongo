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
	// 	"reflect"
	// 	"strconv"
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

	//OutputOpts controls options for how the exported data should be formatted
	OutputOpts *options.OutputFormatOptions

	InputOpts *options.InputOptions

	// for connecting to the db
	SessionProvider *db.SessionProvider
	ExportOutput    ExportOutput
}

//ValidateSettings returns an error if any settings specified on the command line
//were invalid, or nil if they are valid.
func (exp *MongoExport) ValidateSettings() error {
	//TODO - on legacy mongoexport, if -d is blank, it assumes some default database.
	//Do we want to use that same behavior? It seems very odd to assume the DB
	//when only a collection is provided, but that's the behavior of the legacy tools.

	//Namespace must have a valid database and collection
	if exp.ToolOptions.Namespace.DB == "" || exp.ToolOptions.Namespace.Collection == "" {
		return fmt.Errorf("must specify a database and collection")
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

//getOutputWriter returns an io.Writer corresponding to the output location
//specified in the options.
func (exp *MongoExport) getOutputWriter() (io.WriteCloser, error) {
	if exp.OutputOpts.OutputFile != "" {
		//TODO do we care if the file exists already? Overwrite it, or fail?
		file, err := os.Create(exp.OutputOpts.OutputFile)
		if err != nil {
			return nil, err
		}
		return file, err
	}
	return os.Stdout, nil
}

func getDocSource(exp MongoExport) (db.DocSource, error) {
	if exp.ToolOptions.Namespace.DBPath != "" {
		shimPath, err := db.LocateShim()
		if err != nil {
			return nil, err
		}
		bsonTool := db.StorageShim{
			DBPath:     exp.ToolOptions.Namespace.DBPath,
			Database:   exp.ToolOptions.Namespace.DB,
			Collection: exp.ToolOptions.Namespace.Collection,
			Query:      exp.InputOpts.Query,
			Skip:       exp.InputOpts.Skip,
			Limit:      exp.InputOpts.Limit,
			ShimPath:   shimPath,
		}

		iter, _, err := bsonTool.Open()
		if err != nil {
			return nil, err
		}
		return db.NewDecodedBSONSource(iter), nil
	}

	sessionProvider, err := db.InitSessionProvider(exp.ToolOptions)
	if err != nil {
		return nil, err
	}
	session, err := sessionProvider.GetSession()
	if err != nil {
		return nil, err
	}
	collection := session.DB(exp.ToolOptions.Namespace.DB).C(exp.ToolOptions.Namespace.Collection)

	query := map[string]interface{}{}
	if exp.InputOpts != nil && exp.InputOpts.Query != "" {
		var err error
		query, err = getObjectFromArg(exp.InputOpts.Query)
		if err != nil {
			return nil, err
		}
	}

	q := collection.Find(query)

	if exp.InputOpts != nil && exp.InputOpts.Skip > 0 {
		q = q.Skip(exp.InputOpts.Skip)
	}
	if exp.InputOpts != nil && exp.InputOpts.Limit > 0 {
		q = q.Limit(exp.InputOpts.Limit)
	}

	if exp.InputOpts != nil && exp.InputOpts.Sort != "" {
		sortD, err := getSortFromArg(exp.InputOpts.Sort)
		if err != nil {
			return nil, err
		}
		sortFields, err := bsonutil.MakeSortString(sortD)
		if err != nil {
			return nil, err
		}
		q = q.Sort(sortFields...)
	}

	if len(query) == 0 && exp.InputOpts != nil && exp.InputOpts.ForceTableScan != true && exp.InputOpts.Sort == "" {
		q = q.Snapshot()
	}

	cursor := q.Iter()
	return &db.CursorDocSource{cursor, session}, nil
}

//Export executes the entire export operation. It returns an integer of the count
//of documents successfully exported, and a non-nil error if something went wrong
//during the export operation.
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

	//Write headers
	err = exportOutput.WriteHeader()
	if err != nil {
		return 0, err
	}

	var result bson.M

	docsCount := int64(0)
	//Write document content
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

	//Write footers
	err = exportOutput.WriteFooter()
	if err != nil {
		return docsCount, err
	}

	exportOutput.Flush()

	return docsCount, nil
}

//getExportOutput returns an implementation of ExportOutput which can handle
//transforming BSON documents into the appropriate output format and writing
//them to an output stream.
func (exp *MongoExport) getExportOutput(out io.Writer) (ExportOutput, error) {
	if exp.OutputOpts.CSV {
		//TODO what if user specifies *both* --fields and --fieldFile?
		var fields []string
		var err error
		if len(exp.OutputOpts.Fields) > 0 {
			fields = strings.Split(exp.OutputOpts.Fields, ",")
		} else if exp.OutputOpts.FieldFile != "" {
			fields, err = util.GetFieldsFromFile(exp.OutputOpts.FieldFile)
			if err != nil {
				return nil, err
			}
		}
		return NewCSVExportOutput(fields, out), nil
	}
	return NewJSONExportOutput(exp.OutputOpts.JSONArray, out), nil
}

//ExportOutput is an interface that specifies how a document should be formatted
//and written to an output stream
type ExportOutput interface {
	//WriteHeader outputs any pre-record headers that are written once
	//per output file.
	WriteHeader() error

	//WriteRecord writes the given document to the given io.Writer according to
	//the format supported by the underlying ExportOutput implementation.
	ExportDocument(bson.M) error

	//WriteFooter outputs any post-record headers that are written once per
	//output file.
	WriteFooter() error

	//Flush writes any pending data to the underlying I/O stream.
	Flush() error
}

//getObjectFromArg takes an object in extended JSON, and converts it to an object that
//can be passed straight to db.collection.find(...) as a query or sort critera.
//Returns an error if the string is not valid JSON, or extended JSON.
func getObjectFromArg(queryRaw string) (map[string]interface{}, error) {
	parsedJSON := map[string]interface{}{}
	err := sloppyjson.Unmarshal([]byte(queryRaw), &parsedJSON)
	if err != nil {
		return nil, fmt.Errorf("Query is not valid JSON: %v", err)
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

//getSortFromArg takes a sort specification in JSON and returns it as a bson.D
//object which preserves the ordering of the keys as they appear in the input.
func getSortFromArg(queryRaw string) (bson.D, error) {
	parsedJSON := bson.D{}
	err := sloppyjson.Unmarshal([]byte(queryRaw), &parsedJSON)
	if err != nil {
		return nil, fmt.Errorf("Query is not valid JSON: %v", err)
	}
	return parsedJSON, nil
}
