package mongoimport

import (
	"bufio"
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2/bson"
	"io"
	"strconv"
	"strings"
)

const (
	entryDelimiter = '\n'
	tokenSeparator = "\t"
)

// TSVImportInput is a struct that implements the ImportInput interface for a
// TSV input source
type TSVImportInput struct {
	// Fields is a list of field names in the BSON documents to be imported
	Fields []string
	// tsvReader is the underlying reader used to read data in from the TSV
	// or TSV file
	tsvReader *bufio.Reader
	// numProcessed indicates the number of CSV documents processed
	numProcessed int64
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
func (tsvImporter *TSVImportInput) SetHeader(hasHeaderLine bool) (err error) {
	fields, err := validateHeaders(tsvImporter, hasHeaderLine)
	if err != nil {
		return err
	}
	tsvImporter.Fields = fields
	return nil
}

// ImportDocument reads a line of input with the TSV representation of a
// document and returns the BSON equivalent.
func (tsvImporter *TSVImportInput) ImportDocument() (bson.M, error) {
	tsvImporter.numProcessed++
	tsvRecord, err := tsvImporter.tsvReader.ReadString(entryDelimiter)
	if err != nil {
		if err == io.EOF {
			return nil, err
		}
		return nil, fmt.Errorf("read error on entry #%v: %v", tsvImporter.numProcessed, err)
	}
	log.Logf(2, "got line: %v", tsvRecord)

	// strip the trailing '\r\n' from ReadString
	if len(tsvRecord) != 0 {
		tsvRecord = strings.TrimRight(tsvRecord, "\r\n")
	}
	tokens := strings.Split(tsvRecord, tokenSeparator)
	document := bson.M{}
	var key string
	for index, token := range tokens {
		parsedValue := getParsedValue(token)
		if index < len(tsvImporter.Fields) {
			if strings.Contains(tsvImporter.Fields[index], ".") {
				setNestedValue(tsvImporter.Fields[index], parsedValue, document)
			} else {
				document[tsvImporter.Fields[index]] = parsedValue
			}
		} else {
			key = "field" + strconv.Itoa(index)
			if util.StringSliceContains(tsvImporter.Fields, key) {
				return document, fmt.Errorf("Duplicate header name - on %v - for token #%v ('%v') in document #%v",
					key, index+1, parsedValue, tsvImporter.numProcessed)
			}
			document[key] = parsedValue
		}
	}
	return document, nil
}
