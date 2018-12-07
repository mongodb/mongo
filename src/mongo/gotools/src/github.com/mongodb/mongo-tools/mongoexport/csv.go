package mongoexport

import (
	"encoding/csv"
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/json"
	"gopkg.in/mgo.v2/bson"
	"io"
	"reflect"
	"strconv"
	"strings"
)

// type for reflect code
var marshalDType = reflect.TypeOf(bsonutil.MarshalD{})

// CSVExportOutput is an implementation of ExportOutput that writes documents to the output in CSV format.
type CSVExportOutput struct {
	// Fields is a list of field names in the bson documents to be exported.
	// A field can also use dot-delimited modifiers to address nested structures,
	// for example "location.city" or "addresses.0".
	Fields []string

	// NumExported maintains a running total of the number of documents written.
	NumExported int64

	// NoHeaderLine, if set, will export CSV data without a list of field names at the first line
	NoHeaderLine bool

	csvWriter *csv.Writer
}

// NewCSVExportOutput returns a CSVExportOutput configured to write output to the
// given io.Writer, extracting the specified fields only.
func NewCSVExportOutput(fields []string, noHeaderLine bool, out io.Writer) *CSVExportOutput {
	return &CSVExportOutput{
		fields,
		0,
		noHeaderLine,
		csv.NewWriter(out),
	}
}

// WriteHeader writes a comma-delimited list of fields as the output header row.
func (csvExporter *CSVExportOutput) WriteHeader() error {
	if !csvExporter.NoHeaderLine {
		csvExporter.csvWriter.Write(csvExporter.Fields)
		return csvExporter.csvWriter.Error()
	}
	return nil
}

// WriteFooter is a no-op for CSV export formats.
func (csvExporter *CSVExportOutput) WriteFooter() error {
	// no CSV footer
	return nil
}

// Flush writes any pending data to the underlying I/O stream.
func (csvExporter *CSVExportOutput) Flush() error {
	csvExporter.csvWriter.Flush()
	return csvExporter.csvWriter.Error()
}

// ExportDocument writes a line to output with the CSV representation of a document.
func (csvExporter *CSVExportOutput) ExportDocument(document bson.D) error {
	rowOut := make([]string, 0, len(csvExporter.Fields))
	extendedDoc, err := bsonutil.ConvertBSONValueToJSON(document)
	if err != nil {
		return err
	}

	for _, fieldName := range csvExporter.Fields {
		fieldVal := extractFieldByName(fieldName, extendedDoc)
		if fieldVal == nil {
			rowOut = append(rowOut, "")
		} else if reflect.TypeOf(fieldVal) == reflect.TypeOf(bson.M{}) ||
			reflect.TypeOf(fieldVal) == reflect.TypeOf(bson.D{}) ||
			reflect.TypeOf(fieldVal) == marshalDType ||
			reflect.TypeOf(fieldVal) == reflect.TypeOf([]interface{}{}) {
			buf, err := json.Marshal(fieldVal)
			if err != nil {
				rowOut = append(rowOut, "")
			} else {
				rowOut = append(rowOut, string(buf))
			}
		} else {
			rowOut = append(rowOut, fmt.Sprintf("%v", fieldVal))
		}
	}
	csvExporter.csvWriter.Write(rowOut)
	csvExporter.NumExported++
	return csvExporter.csvWriter.Error()
}

// extractFieldByName takes a field name and document, and returns a value representing
// the value of that field in the document in a format that can be printed as a string.
// It will also handle dot-delimited field names for nested arrays or documents.
func extractFieldByName(fieldName string, document interface{}) interface{} {
	dotParts := strings.Split(fieldName, ".")
	var subdoc interface{} = document

	for _, path := range dotParts {
		docValue := reflect.ValueOf(subdoc)
		if !docValue.IsValid() {
			return ""
		}
		docType := docValue.Type()
		docKind := docType.Kind()
		if docKind == reflect.Map {
			subdocVal := docValue.MapIndex(reflect.ValueOf(path))
			if subdocVal.Kind() == reflect.Invalid {
				return ""
			}
			subdoc = subdocVal.Interface()
		} else if docKind == reflect.Slice {
			if docType == marshalDType {
				// dive into a D as a document
				asD := bson.D(subdoc.(bsonutil.MarshalD))
				var err error
				subdoc, err = bsonutil.FindValueByKey(path, &asD)
				if err != nil {
					return ""
				}
			} else {
				//  check that the path can be converted to int
				arrayIndex, err := strconv.Atoi(path)
				if err != nil {
					return ""
				}
				// bounds check for slice
				if arrayIndex < 0 || arrayIndex >= docValue.Len() {
					return ""
				}
				subdocVal := docValue.Index(arrayIndex)
				if subdocVal.Kind() == reflect.Invalid {
					return ""
				}
				subdoc = subdocVal.Interface()
			}
		} else {
			// trying to index into a non-compound type - just return blank.
			return ""
		}
	}
	return subdoc
}
