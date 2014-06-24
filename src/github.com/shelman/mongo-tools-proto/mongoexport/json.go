package mongoexport

import (
	"encoding/json"
	"io"
	"labix.org/v2/mgo/bson"
)

type JSONExportOutput struct {
	//ArrayOutput when set to true indicates that the output should be written
	//as a JSON array, where each document is an element in the array.
	ArrayOutput bool
	Encoder     *json.Encoder
	Out         io.Writer
	NumExported int64
}

func NewJSONExportOutput(arrayOutput bool, out io.Writer) *JSONExportOutput {
	return &JSONExportOutput{
		arrayOutput,
		json.NewEncoder(out),
		out,
		0,
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
		_, err := jsonExporter.Out.Write([]byte(JSON_ARRAY_END + "\n"))
		//TODO check # bytes written?
		if err != nil {
			return err
		}
	}
	return nil
}

func (jsonExporter *JSONExportOutput) ExportDocument(document bson.M) error {
	if jsonExporter.ArrayOutput {
		if jsonExporter.NumExported >= 1 {
			jsonExporter.Out.Write([]byte(","))
		}
		jsonOut, err := json.Marshal(getExtendedJsonRepr(document))
		if err != nil {
			return nil
		} else {
			jsonExporter.Out.Write(jsonOut)
		}
	} else {
		err := jsonExporter.Encoder.Encode(getExtendedJsonRepr(document))
		if err != nil {
			return err
		}
	}
	jsonExporter.NumExported++
	return nil
}
