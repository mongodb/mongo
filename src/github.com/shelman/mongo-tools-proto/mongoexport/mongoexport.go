package mongoexport

import (
	"encoding/json"
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/db"
	commonopts "github.com/shelman/mongo-tools-proto/common/options"
	"github.com/shelman/mongo-tools-proto/mongoexport/options"
	"io"
	"labix.org/v2/mgo/bson"
	"os"
)

const (
	JSON_ARRAY_START = "["
	JSON_ARRAY_END   = "]"
)

// Wrapper for mongoexport functionality
type MongoExport struct {
	// generic mongo tool options
	ToolOptions *commonopts.ToolOptions

	OutputOpts *options.OutputFormatOptions

	// for connecting to the db
	SessionProvider *db.SessionProvider
	ExportOutput    ExportOutput
}

func (exp *MongoExport) ValidateSettings() error {

	//TODO - on legacy mongoexport, if -d is blank, it assumes some default database.
	//Do we want to use that same behavior? seems weird to assume a specific database
	//when only a collection is provided.

	//Namespace must have a valid database and collection
	if exp.ToolOptions.Namespace.DB == "" || exp.ToolOptions.Namespace.Collection == "" {
		return fmt.Errorf("must specify a database and collection.")
	}
	return nil
}

func (exp *MongoExport) getOutputWriter() (io.Writer, error) {
	if exp.OutputOpts.OutputFile != "" {
		//TODO do we care if the file exists already?
		file, err := os.Create(exp.OutputOpts.OutputFile)
		if err != nil {
			return nil, err
		}
		return file, err
	} else {
		return os.Stdout, nil
	}
}

func (exp *MongoExport) Export() error {
	session, err := exp.SessionProvider.GetSession()
	if err != nil {
		return err
	}

	out, err := exp.getOutputWriter()
	if err != nil {
		return err
	}

	exportOutput := exp.getExportOutput(out)

	collection := session.DB(exp.ToolOptions.Namespace.DB).C(exp.ToolOptions.Namespace.Collection)

	//TODO get a real query
	query := bson.M{}
	cursor := collection.Find(query).Iter()
	defer cursor.Close()

	//Write headers
	err = exportOutput.WriteHeader()
	if err != nil {
		return err
	}

	var result bson.M
	//Write document content
	for cursor.Next(&result) {
		exportOutput.ExportDocument(result)
	}

	//Write footers
	err = exportOutput.WriteFooter()
	if err != nil {
		return err
	}

	return nil
}

func (exp *MongoExport) getExportOutput(out io.Writer) ExportOutput {
	//if !OutputOpts.CSV {
	return NewJSONExportOutput(exp.OutputOpts.JSONArray, out)
	//}

}

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
}

type JSONExportOutput struct {
	//ArrayOutput when set to true indicates that the output should be written
	//as a JSON array, where each document is an element in the array.
	ArrayOutput bool
	Encoder     *json.Encoder
	Out         io.Writer
}

func NewJSONExportOutput(arrayOutput bool, out io.Writer) *JSONExportOutput {
	return &JSONExportOutput{
		arrayOutput,
		json.NewEncoder(out),
		out,
	}
}

func (jsonExporter *JSONExportOutput) WriteHeader() error {
	if jsonExporter.ArrayOutput {
		//TODO check # bytes written?
		_, err := jsonExporter.Out.Write([]byte(JSON_ARRAY_START))
		if err != nil {
			return err
		}
	}
	return nil
}

func (jsonExporter *JSONExportOutput) WriteFooter() error {
	if jsonExporter.ArrayOutput {
		_, err := jsonExporter.Out.Write([]byte(JSON_ARRAY_END))
		//TODO check # bytes written?
		if err != nil {
			return err
		}
	}
	return nil
}

func (jsonExporter *JSONExportOutput) ExportDocument(document bson.M) error {
	return jsonExporter.Encoder.Encode(document)
}
