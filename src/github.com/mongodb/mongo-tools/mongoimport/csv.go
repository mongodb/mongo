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
	// NumImported indicates the number of CSV documents processed
	NumProcessed int64
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
	sort.Sort(sort.StringSlice(headers))
	for index, header := range headers {
		csvImporter.Fields = append(csvImporter.Fields, header)
		if strings.Contains(header, ".") && header[len(header)-1] == '.' {
			return fmt.Errorf("header '%v' can not end in '.'", header)
		}
		for _, latterHeader := range headers[index+1:] {
			if strings.HasPrefix(latterHeader, header) && (strings.Contains(header, ".") || strings.Contains(latterHeader, ".")) {
				return fmt.Errorf("incompatible headers found: '%v' and '%v", header, latterHeader)
			}
			if header == latterHeader {
				return fmt.Errorf("headers can not be identical: '%v' and '%v", header, latterHeader)
			}
		}
	}
	return nil
}

// ImportDocument reads a line of input with the CSV representation of a doc and
// returns the BSON equivalent.
func (csvImporter *CSVImportInput) ImportDocument() (bson.M, error) {
	document := bson.M{}
	csvImporter.NumProcessed++
	tokens, err := csvImporter.csvReader.Read()
	if err != nil {
		if err == io.EOF {
			return nil, err
		}
		return nil, fmt.Errorf("read error on entry #%v: %v", csvImporter.NumProcessed, err)
	}
	var key string
	for index, token := range tokens {
		parsedValue := getParsedValue(token)
		if index < len(csvImporter.Fields) {
			if strings.Contains(csvImporter.Fields[index], ".") {
				setUpsertValue(csvImporter.Fields[index], &document, parsedValue)
			} else {
				document[csvImporter.Fields[index]] = parsedValue
			}
		} else {
			key = "field" + strconv.Itoa(index)
			if util.StringSliceContains(csvImporter.Fields, key) {
				return document, fmt.Errorf("key collision for token #%v ('%v') on document #%v", index+1, parsedValue, csvImporter.NumProcessed)
			}
			document[key] = parsedValue
		}
	}
	return document, nil
}

// setUpsertValue takes a nested field - in the form "a.b.c"
// and sets the value passed in the nested field in the document
func setUpsertValue(field string, doc *bson.M, value interface{}) bson.M {
	document := *doc
	index := strings.Index(field, ".")
	if index == -1 {
		if document == nil {
			return bson.M{field: value}
		}
		document[field] = value
		return document
	}
	left := field[0:index]
	if document[left] == nil {
		document[left] = setUpsertValue(field[index+1:], &bson.M{}, value)
	} else {
		subDoc := document[left].(bson.M)
		document[left] = setUpsertValue(field[index+1:], &subDoc, value)
	}
	return document
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
