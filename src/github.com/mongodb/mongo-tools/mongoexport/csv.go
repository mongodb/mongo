package mongoexport

import (
	"encoding/csv"
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"gopkg.in/mgo.v2/bson"
	"io"
	"reflect"
	"strconv"
	"strings"
)

type CSVExportOutput struct {
	//Fields is a list of field names in the bson documents to be exported.
	//A field can also use dot-delimited modifiers to address nested structures,
	//for example "location.city" or "addresses.0"
	Fields []string

	//NumExported maintains a running total of the number of documents written
	NumExported int64

	csvWriter *csv.Writer
}

//NewCSVExportOutput returns a CSVExportOutput configured to write output to the
//given io.Writer, extracting the specified fields only.
func NewCSVExportOutput(fields []string, out io.Writer) *CSVExportOutput {
	return &CSVExportOutput{
		fields,
		0,
		csv.NewWriter(out),
	}
}

//WriteHeader writes a comma-delimited list of fields as the output header row
func (csvExporter *CSVExportOutput) WriteHeader() error {
	return csvExporter.csvWriter.Write(csvExporter.Fields)
}

func (csvExporter *CSVExportOutput) WriteFooter() error {
	//no csv footer
	return nil
}

func (csvExporter *CSVExportOutput) Flush() error {
	csvExporter.csvWriter.Flush()
	return csvExporter.csvWriter.Error()
}

//ExportDocument writes a line to output with the CSV representation of a doc.
func (csvExporter *CSVExportOutput) ExportDocument(document bson.M) error {
	rowOut := make([]string, 0, len(csvExporter.Fields))
	extendedDoc, err := bsonutil.ConvertBSONValueToJSON(document)
	if err != nil {
		return err
	}
	for _, fieldName := range csvExporter.Fields {
		fieldVal, err := extractFieldByName(fieldName, extendedDoc)
		if err != nil {
			return nil
		}
		rowOut = append(rowOut, fmt.Sprintf("%s", fieldVal))
	}
	err = csvExporter.csvWriter.Write(rowOut)
	if err != nil {
		return err
	}
	csvExporter.NumExported++
	return nil
}

//extractFieldByName takes a field name and document, and returns a value representing
//the value of that field in the document in a format that can be printed as a string.
//It will also handle dot-delimited field names for nested arrays or documents.
func extractFieldByName(fieldName string, document interface{}) (interface{}, error) {
	dotParts := strings.Split(fieldName, ".")
	var subdoc interface{} = document

	for _, path := range dotParts {
		docValue := reflect.ValueOf(subdoc)
		docType := docValue.Type()
		docKind := docType.Kind()
		//fmt.Println("dockind is", docKind, "subdoc", subdoc)
		if docKind == reflect.Map {
			subdocVal := docValue.MapIndex(reflect.ValueOf(path))
			if subdocVal.Kind() == reflect.Invalid {
				return "", nil
			}
			subdoc = subdocVal.Interface()
		} else if docKind == reflect.Slice {
			// check that the path can be converted to int
			arrayIndex, err := strconv.Atoi(path)
			if err != nil {
				return "", nil
			}
			//bounds check for slice
			if arrayIndex < 0 || arrayIndex >= docValue.Len() {
				return "", nil
			}
			subdocVal := docValue.Index(arrayIndex)
			if subdocVal.Kind() == reflect.Invalid {
				return "", nil
			}
			subdoc = subdocVal.Interface()
		} else {
			//trying to index into a non-compound type - just return blank.
			return "", nil
		}
	}
	/*
		finalValKind := reflect.TypeOf(subdoc)
		if finalValKind == reflect.Map || finalValKind == reflect.Slice {
			subdocAsJson, err := json.Marshal(subdoc)
			if err != nil {
				return nil, err
			}
			return string(subdocAsJson), nil
		}*/

	return subdoc, nil
}
