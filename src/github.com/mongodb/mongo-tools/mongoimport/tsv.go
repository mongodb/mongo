package mongoimport

import (
	"bufio"
	"gopkg.in/mgo.v2/bson"
	"io"
	"strconv"
	"strings"
)

const (
	entryDelimiter = '\n'
	tokenSeparator = "\t"
)

// TODO: TOOLS-64, TOOLS-70 for TSV

// TSVImportInput is a struct that implements the ImportInput interface for a
// TSV input source
type TSVImportInput struct {
	// Fields is a list of field names in the BSON documents to be imported
	Fields []string
	// tsvReader is the underlying reader used to read data in from the TSV
	// or TSV file
	tsvReader *bufio.Reader
}

// NewTSVImportInput returns a TSVImportInput configured to read input from the
// given io.Reader, extracting the specified fields only.
func NewTSVImportInput(fields []string, in io.Reader) *TSVImportInput {
	return &TSVImportInput{
		Fields:    fields,
		tsvReader: bufio.NewReader(in),
	}
}

// SetHeader sets the header field for a TSV
func (tsvImporter *TSVImportInput) SetHeader() error {
	headers, err := tsvImporter.tsvReader.ReadString(entryDelimiter)
	if err != nil {
		return err
	}
	tokenizedHeaders := strings.Split(headers, tokenSeparator)

	for _, header := range tokenizedHeaders {
		tsvImporter.Fields = append(tsvImporter.Fields,
			strings.TrimSpace(header))
	}
	return nil
}

// ImportDocument reads a line of input with the TSV representation of a doc and
// returns the BSON equivalent.
func (tsvImporter *TSVImportInput) ImportDocument() (bson.M, error) {
	tsvRecord, err := tsvImporter.tsvReader.ReadString(entryDelimiter)
	if err != nil {
		return nil, err
	}
	document := bson.M{}

	// strip the trailing '\r\n' from ReadString
	if len(tsvRecord) != 0 {
		tsvRecord = strings.TrimRight(tsvRecord, "\r\n")
	}
	tokens := strings.Split(tsvRecord, tokenSeparator)
	for index, token := range tokens {
		parsedValue := getParsedValue(token)
		if index < len(tsvImporter.Fields) {
			document[tsvImporter.Fields[index]] = parsedValue
		} else {
			document["field"+strconv.Itoa(index)] = parsedValue
		}
	}
	return document, nil
}
