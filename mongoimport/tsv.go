package mongoimport

import (
	"bufio"
	"fmt"
	"gopkg.in/mgo.v2/bson"
	"io"
	"strings"
)

const (
	entryDelimiter = '\n'
	tokenSeparator = "\t"
)

// TSVInputReader is a struct that implements the InputReader interface for a
// TSV input source
type TSVInputReader struct {
	// Fields is a list of field names in the BSON documents to be imported
	Fields []string
	// tsvReader is the underlying reader used to read data in from the TSV
	// or TSV file
	tsvReader *bufio.Reader
	// tsvRecord stores each line of input we read from the underlying reader
	tsvRecord string
	// numProcessed tracks the number of TSV records processed by the underlying
	// reader
	numProcessed uint64
}

// TSVConvertibleDoc implements the ConvertibleDoc interface for TSV input
type TSVConvertibleDoc struct {
	fields       []string
	data         string
	numProcessed *uint64
}

// NewTSVInputReader returns a TSVInputReader configured to read input from the
// given io.Reader, extracting the specified fields only.
func NewTSVInputReader(fields []string, in io.Reader) *TSVInputReader {
	return &TSVInputReader{
		Fields:       fields,
		tsvReader:    bufio.NewReader(in),
		numProcessed: uint64(0),
	}
}

// SetHeader sets the header field for a TSV
func (tsvInputReader *TSVInputReader) SetHeader(hasHeaderLine bool) (err error) {
	fields, err := validateHeaders(tsvInputReader, hasHeaderLine)
	if err != nil {
		return err
	}
	tsvInputReader.Fields = fields
	return nil
}

// GetHeaders returns the current header fields for a TSV importer
func (tsvInputReader *TSVInputReader) GetHeaders() []string {
	return tsvInputReader.Fields
}

// ReadHeadersFromSource reads the header field from the TSV importer's reader
func (tsvInputReader *TSVInputReader) ReadHeadersFromSource() ([]string, error) {
	unsortedHeaders := []string{}
	stringHeaders, err := tsvInputReader.tsvReader.ReadString(entryDelimiter)
	if err != nil {
		return nil, err
	}
	tokenizedHeaders := strings.Split(stringHeaders, tokenSeparator)
	for _, header := range tokenizedHeaders {
		unsortedHeaders = append(unsortedHeaders, strings.TrimSpace(header))
	}
	return unsortedHeaders, nil
}

// StreamDocument takes in two channels: it sends processed documents on the
// readChan channel and if any error is encountered, the error is sent on the
// errChan channel. It keeps reading from the underlying input source until it
// hits EOF or an error. If ordered is true, it streams the documents in which
// the documents are read
func (tsvInputReader *TSVInputReader) StreamDocument(ordered bool, readChan chan bson.D, errChan chan error) {
	tsvRecordChan := make(chan ConvertibleDoc, numDecodingWorkers)
	var err error

	go func() {
		for {
			tsvInputReader.tsvRecord, err = tsvInputReader.tsvReader.ReadString(entryDelimiter)
			if err != nil {
				close(tsvRecordChan)
				if err == io.EOF {
					errChan <- err
				} else {
					tsvInputReader.numProcessed++
					errChan <- fmt.Errorf("read error on entry #%v: %v", tsvInputReader.numProcessed, err)
				}
				return
			}
			tsvRecordChan <- TSVConvertibleDoc{
				fields:       tsvInputReader.Fields,
				data:         tsvInputReader.tsvRecord,
				numProcessed: &tsvInputReader.numProcessed,
			}
			tsvInputReader.numProcessed++
		}
	}()
	streamDocuments(ordered, tsvRecordChan, readChan, errChan)
}

// This is required to satisfy the ConvertibleDoc interface for TSV input. It
// does TSV-specific processing to convert the TSVConvertibleDoc to a bson.D
func (tsvConvertibleDoc TSVConvertibleDoc) Convert() (bson.D, error) {
	tsvTokens := strings.Split(
		strings.TrimRight(tsvConvertibleDoc.data, "\r\n"),
		tokenSeparator,
	)
	return tokensToBSON(
		tsvConvertibleDoc.fields,
		tsvTokens,
		*tsvConvertibleDoc.numProcessed,
	)
}
