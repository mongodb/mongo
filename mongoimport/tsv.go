package mongoimport

import (
	"bufio"
	"fmt"
	"github.com/mongodb/mongo-tools/common/log"
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

	// numProcessed tracks the number of TSV records processed by the underlying reader
	numProcessed uint64

	// numDecoders is the number of concurrent goroutines to use for decoding
	numDecoders int
}

// TSVConvertibleDoc implements the ConvertibleDoc interface for TSV input
type TSVConvertibleDoc struct {
	fields       []string
	data         string
	numProcessed *uint64
}

// NewTSVInputReader returns a TSVInputReader configured to read input from the
// given io.Reader, extracting the specified fields only.
func NewTSVInputReader(fields []string, in io.Reader, numDecoders int) *TSVInputReader {
	return &TSVInputReader{
		Fields:       fields,
		tsvReader:    bufio.NewReader(in),
		numProcessed: uint64(0),
		numDecoders:  numDecoders,
	}
}

// SetFields sets the import fields for a TSV importer
func (tsvInputReader *TSVInputReader) SetFields(hasHeaderLine bool) (err error) {
	if hasHeaderLine {
		fields, err := tsvInputReader.ReadHeaderFromSource()
		if err != nil {
			return err
		}
		tsvInputReader.Fields = fields
	}
	if err = validateFields(tsvInputReader.Fields); err != nil {
		return err
	}
	if len(tsvInputReader.Fields) == 1 {
		log.Logf(log.Info, "using field: %v", tsvInputReader.Fields[0])
	} else {
		log.Logf(log.Info, "using fields: %v", strings.Join(tsvInputReader.Fields, ","))
	}
	return nil
}

// ReadHeaderFromSource reads the header from the TSV importer's reader
func (tsvInputReader *TSVInputReader) ReadHeaderFromSource() ([]string, error) {
	fields := []string{}
	header, err := tsvInputReader.tsvReader.ReadString(entryDelimiter)
	if err != nil {
		return nil, err
	}
	tokenizedFields := strings.Split(header, tokenSeparator)
	for _, field := range tokenizedFields {
		fields = append(fields, strings.TrimRight(field, "\r\n"))
	}
	return fields, nil
}

// StreamDocument takes in two channels: it sends processed documents on the
// readDocChan channel and if any error is encountered, the error is sent on the
// errChan channel. It keeps reading from the underlying input source until it
// hits EOF or an error. If ordered is true, it streams the documents in which
// the documents are read
func (tsvInputReader *TSVInputReader) StreamDocument(ordered bool, readDocChan chan bson.D, errChan chan error) {
	tsvRecordChan := make(chan ConvertibleDoc, tsvInputReader.numDecoders)
	go func() {
		var err error
		for {
			tsvInputReader.tsvRecord, err = tsvInputReader.tsvReader.ReadString(entryDelimiter)
			if err != nil {
				close(tsvRecordChan)
				if err != io.EOF {
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
	errChan <- streamDocuments(ordered, tsvInputReader.numDecoders, tsvRecordChan, readDocChan)
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
