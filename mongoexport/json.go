package mongoexport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/json"
	"gopkg.in/mgo.v2/bson"
	"io"
	"bytes"
)

// JSONExportOutput is an implementation of ExportOutput that writes documents
// to the output in JSON format.
type JSONExportOutput struct {
	// ArrayOutput when set to true indicates that the output should be written
	// as a JSON array, where each document is an element in the array.
	// Pretty when set to true indicates that the output will be written in pretty() mode
	ArrayOutput bool
	PrettyOutput bool
	Encoder     *json.Encoder
	Out         io.Writer
	NumExported int64
}

// NewJSONExportOutput creates a new JSONExportOutput in array mode if specified,
// configured to write data to the given io.Writer
func NewJSONExportOutput(arrayOutput bool, prettyOutput bool, out io.Writer) *JSONExportOutput {
	return &JSONExportOutput{
		arrayOutput,
		prettyOutput,
		json.NewEncoder(out),
		out,
		0,
	}
}

// WriteHeader writes the opening square bracket if in array mode, otherwise it
// behaves as a no-op.
func (jsonExporter *JSONExportOutput) WriteHeader() error {
	if jsonExporter.ArrayOutput {
		// TODO check # bytes written?
		_, err := jsonExporter.Out.Write([]byte(JSON_ARRAY_START))
		if err != nil {
			return err
		}
	}
	return nil
}

// WriteFooter writes the closing square bracket if in array mode, otherwise it
// behaves as a no-op.
func (jsonExporter *JSONExportOutput) WriteFooter() error {
	if jsonExporter.ArrayOutput {
		_, err := jsonExporter.Out.Write([]byte(JSON_ARRAY_END + "\n"))
		// TODO check # bytes written?
		if err != nil {
			return err
		}
	}
	return nil
}

func (jsonExporter *JSONExportOutput) Flush() error {
	return nil
}

// ExportDocument converts the given document to extended json, and writes it
// to the output.
func (jsonExporter *JSONExportOutput) ExportDocument(document bson.M) error {
	if jsonExporter.ArrayOutput || jsonExporter.PrettyOutput {
		if jsonExporter.NumExported >= 1 {
			if jsonExporter.ArrayOutput {
				jsonExporter.Out.Write([]byte(","))
			}
			if jsonExporter.PrettyOutput {
				jsonExporter.Out.Write([]byte("\n"))
			}
		}
		extendedDoc, err := bsonutil.ConvertBSONValueToJSON(document)
		if err != nil {
			return err
		}
		jsonOut, err := json.Marshal(extendedDoc)
		if err != nil {
			return fmt.Errorf("Error converting BSON to extended JSON: %v", err)
		}
		if jsonExporter.PrettyOutput{
			var jsonFormatted bytes.Buffer
			json.Indent(&jsonFormatted, jsonOut, "", "\t")
			jsonExporter.Out.Write(jsonFormatted.Bytes())
		}
		if jsonExporter.ArrayOutput {
			jsonExporter.Out.Write(jsonOut)
		}
	} else {
		extendedDoc, err := bsonutil.ConvertBSONValueToJSON(document)
		if err != nil {
			return err
		}
		err = jsonExporter.Encoder.Encode(extendedDoc)
		if err != nil {
			return err
		}
	}
	jsonExporter.NumExported++
	return nil
}
