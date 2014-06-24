package mongoexport

import (
	"bufio"
	"encoding/csv"
	//"encoding/json"
	"fmt"
	"io"

	"labix.org/v2/mgo/bson"
	"os"
	"reflect"
	"strconv"
	"strings"
)

type CSVExportOutput struct {
	Fields      []string
	NumExported int64
	csvWriter   *csv.Writer
}

func getFieldsFromFile(path string) ([]string, error) {
	fieldFileReader, err := os.Open(path)
	if err != nil {
		return nil, err
	}

	var fields []string
	fieldScanner := bufio.NewScanner(fieldFileReader)
	for fieldScanner.Scan() {
		fields = append(fields, fieldScanner.Text())
	}
	if err := fieldScanner.Err(); err != nil {
		return nil, err
	}
	return fields, nil
}

func NewCSVExportOutput(fields []string, out io.Writer) *CSVExportOutput {
	return &CSVExportOutput{
		fields,
		0,
		csv.NewWriter(out),
	}
}

func (csvExporter *CSVExportOutput) WriteHeader() error {
	return csvExporter.csvWriter.Write(csvExporter.Fields)
}

func (csvExporter *CSVExportOutput) WriteFooter() error {
	csvExporter.csvWriter.Flush()
	return csvExporter.csvWriter.Error()
}

func (csvExporter *CSVExportOutput) ExportDocument(document bson.M) error {
	rowOut := make([]string, 0, len(csvExporter.Fields))
	extendedDoc := getExtendedJsonRepr(document)
	for _, fieldName := range csvExporter.Fields {
		fieldVal, err := extractFieldByName(fieldName, extendedDoc)
		if err != nil {
			return nil
		}
		rowOut = append(rowOut, fmt.Sprintf("%s", fieldVal))
	}
	err := csvExporter.csvWriter.Write(rowOut)
	if err != nil {
		return err
	}
	csvExporter.NumExported++
	return nil
}

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
			} else {
				subdoc = subdocVal.Interface()
			}
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
			} else {
				subdoc = subdocVal.Interface()
			}
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
