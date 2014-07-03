package mongoexport

import (
	"encoding/json"
	"github.com/shelman/mongo-tools-proto/common/bson_ext"
	"io"
	"labix.org/v2/mgo/bson"
)

//JSONExportOutput is an implementation of ExportOutput that writes documents
//to the output in JSON format.
type JSONExportOutput struct {
	//ArrayOutput when set to true indicates that the output should be written
	//as a JSON array, where each document is an element in the array.
	ArrayOutput bool
	Encoder     *json.Encoder
	Out         io.Writer
	NumExported int64
}

//NewJSONExportOutput creates a new JSONExportOutput in array mode if specified,
//configured to write data to the given io.Writer
func NewJSONExportOutput(arrayOutput bool, out io.Writer) *JSONExportOutput {
	return &JSONExportOutput{
		arrayOutput,
		json.NewEncoder(out),
		out,
		0,
	}
}

//WriteHeader writes the opening square bracket if in array mode, otherwise it
//behaves as a no-op.
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

//WriteFooter writes the closing square bracket if in array mode, otherwise it
//behaves as a no-op.
func (jsonExporter *JSONExportOutput) WriteFooter() error {
	if jsonExporter.ArrayOutput {
		_, err := jsonExporter.Out.Write([]byte(JSON_ARRAY_END + "\n"))
		//TODO check # bytes written?
		if err != nil {
			return err
		}
	}
	return nil
}

func (jsonExporter *JSONExportOutput) Flush() error {
	return nil
}

//ExportDocument converts the given document to extended json, and writes it
//to the output.
func (jsonExporter *JSONExportOutput) ExportDocument(document bson.M) error {
	if jsonExporter.ArrayOutput {
		if jsonExporter.NumExported >= 1 {
			jsonExporter.Out.Write([]byte(","))
		}
		jsonOut, err := json.Marshal(bson_ext.GetExtendedBSON(document))
		if err != nil {
			return nil
		} else {
			jsonExporter.Out.Write(jsonOut)
		}
	} else {
		err := jsonExporter.Encoder.Encode(bson_ext.GetExtendedBSON(document))
		if err != nil {
			return err
		}
	}
	jsonExporter.NumExported++
	return nil
}
