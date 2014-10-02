package mongoimport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongoimport/csv"
	"gopkg.in/mgo.v2/bson"
	"io"
	"sort"
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
	// NumImported indicates the number of CSV documents successfully parsed from
	// the CSV input source
	NumImported int64
	// headerLine is a boolean that indicates if the import input has a header line
	headerLine bool
}

// NewCSVImportInput returns a CSVImportInput configured to read input from the
// given io.Reader, extracting the specified fields only.
func NewCSVImportInput(fields []string, headerLine bool, in io.Reader) *CSVImportInput {
	csvReader := csv.NewReader(in)
	// allow variable number of fields in document
	csvReader.FieldsPerRecord = -1
	csvReader.TrimLeadingSpace = true
	return &CSVImportInput{
		Fields:     fields,
		headerLine: headerLine,
		csvReader:  csvReader,
	}
}

// SetHeader sets the header field for a CSV
func (csvImporter *CSVImportInput) SetHeader() (err error) {
	unsortedHeaders := []string{}
	// NOTE: if --headerline was passed on the command line, we will
	// attempt to read headers from the input source - even if --fields
	// or --fieldFile is supplied.
	// TODO: add validation for this case
	if csvImporter.headerLine {
		unsortedHeaders, err = csvImporter.csvReader.Read()
		if err != nil {
			return err
		}
	} else {
		unsortedHeaders = csvImporter.Fields
		csvImporter.Fields = []string{}
	}
	headers := make([]string, len(unsortedHeaders), len(unsortedHeaders))
	copy(headers, unsortedHeaders)
	sort.Sort(sort.StringSlice(headers))
	for index, header := range headers {
		if strings.HasSuffix(header, ".") || strings.HasPrefix(header, ".") {
			return fmt.Errorf("header '%v' can not start or end in '.'", header)
		}
		if strings.Contains(header, "..") {
			return fmt.Errorf("header '%v' can not contain consecutive '.' characters", header)
		}
		// NOTE: since headers is sorted, this check ensures that no header
		// is incompatible with another one that occurs further down the list.
		// meant to prevent cases where we have headers like "a" and "a.c"
		for _, latterHeader := range headers[index+1:] {
			if strings.HasPrefix(latterHeader, header) && (strings.Contains(header, ".") || strings.Contains(latterHeader, ".")) {
				return fmt.Errorf("incompatible headers found: '%v' and '%v", header, latterHeader)
			}
			// NOTE: this means we will not support imports that have fields like
			// a,a - since this is invalid in MongoDB
			if header == latterHeader {
				return fmt.Errorf("headers can not be identical: '%v' and '%v", header, latterHeader)
			}
		}
		csvImporter.Fields = append(csvImporter.Fields, unsortedHeaders[index])
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
	var key string
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
				csvImporter.NumImported++
				return document, fmt.Errorf("key collision for token #%v ('%v') on document #%v", index+1, parsedValue, csvImporter.NumImported)
			}
			document[key] = parsedValue
		}
	}
	csvImporter.NumImported++
	return document, nil
}

// setNestedValue takes a nested field - in the form "a.b.c" -
// its associated value, and a document. It then assigns that
// value to the appropriate nested field within the document
func setNestedValue(field string, value interface{}, document bson.M) {
	index := strings.Index(field, ".")
	if index == -1 {
		document[field] = value
		return
	}
	left := field[0:index]
	subDocument := bson.M{}
	if document[left] != nil {
		subDocument = document[left].(bson.M)
	}
	setNestedValue(field[index+1:], value, subDocument)
	document[left] = subDocument
	return
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
