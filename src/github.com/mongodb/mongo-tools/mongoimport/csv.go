package mongoimport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongoimport/csv"
	"gopkg.in/mgo.v2/bson"
	"io"
	"strconv"
	"strings"
)

// CSVImportInput is a struct that implements the ImportInput interface for a
// CSV input source
type CSVImportInput struct {
	// Fields is a list of field names in the BSON documents to be imported
	Fields []string
	// csvReader is the underlying reader used to read data in from the CSV
	// or TSV file
	csvReader *csv.Reader
	// numProcessed indicates the number of CSV documents processed
	numProcessed int64
}

// NewCSVImportInput returns a CSVImportInput configured to read input from the
// given io.Reader, extracting the specified fields only.
func NewCSVImportInput(fields []string, in io.Reader) *CSVImportInput {
	csvReader := csv.NewReader(in)
	// allow variable number of fields in document
	csvReader.FieldsPerRecord = -1
	csvReader.TrimLeadingSpace = true
	return &CSVImportInput{
		Fields:    fields,
		csvReader: csvReader,
	}
}

// SetHeader sets the header field for a CSV
func (csvImporter *CSVImportInput) SetHeader(hasHeaderLine bool) (err error) {
	fields, err := validateHeaders(csvImporter, hasHeaderLine)
	if err != nil {
		return err
	}
	csvImporter.Fields = fields
	return nil
}

// ImportDocument reads a line of input with the CSV representation of a doc and
// returns the BSON equivalent.
func (csvImporter *CSVImportInput) ImportDocument() (bson.M, error) {
	csvImporter.numProcessed++
	tokens, err := csvImporter.csvReader.Read()
	if err != nil {
		if err == io.EOF {
			return nil, err
		}
		return nil, fmt.Errorf("read error on entry #%v: %v", csvImporter.numProcessed, err)
	}
	log.Logf(2, "got line: %v", strings.Join(tokens, ","))
	var key string
	document := bson.M{}
	for index, token := range tokens {
		parsedValue := getParsedValue(token)
		if index < len(csvImporter.Fields) {
			// for nested fields - in the form "a.b.c", ensure
			// that the value is set accordingly
			if strings.Contains(csvImporter.Fields[index], ".") {
				setNestedValue(csvImporter.Fields[index], parsedValue, document)
			} else {
				document[csvImporter.Fields[index]] = parsedValue
			}
		} else {
			key = "field" + strconv.Itoa(index)
			if util.StringSliceContains(csvImporter.Fields, key) {
				return document, fmt.Errorf("Duplicate header name - on %v - for token #%v ('%v') in document #%v",
					key, index+1, parsedValue, csvImporter.numProcessed)
			}
			document[key] = parsedValue
		}
	}
	return document, nil
}
