package mongoimport

import (
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
func (csvImporter *CSVImportInput) SetHeader() error {
	headers, err := csvImporter.csvReader.Read()
	if err != nil {
		return err
	}

	for _, header := range headers {
		csvImporter.Fields = append(csvImporter.Fields, header)
	}
	return nil
}

// ImportDocument reads a line of input with the CSV representation of a doc and
// returns the BSON equivalent.
func (csvImporter *CSVImportInput) ImportDocument() (bson.M, error) {
	tokens, err := csvImporter.csvReader.Read()
	if err != nil {
		return nil, err
	}
	document := bson.M{}
	for index, token := range tokens {
		parsedValue := getParsedValue(token)
		if index < len(csvImporter.Fields) {
			document[csvImporter.Fields[index]] = parsedValue
		} else {
			document["field"+strconv.Itoa(index)] = parsedValue
		}
	}
	return document, nil
}

// getParsedValue returns the appropriate concrete type for the given token
// it first attempts to convert it to an int, if that doesn't succeed, it
// attempts conversion to a float, if that doesn't succeed, it returns the
// token as is.
func getParsedValue(token string) interface{} {
	parsedInt, err := strconv.Atoi(strings.Trim(token, " "))
	if err == nil {
		return parsedInt
	}
	parsedFloat, err := strconv.ParseFloat(strings.Trim(token, " "), 64)
	if err == nil {
		return parsedFloat
	}
	return token
}
