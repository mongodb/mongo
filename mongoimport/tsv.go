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
// TSV input source.
type TSVInputReader struct {
	// fields is a list of field names in the BSON documents to be imported
	fields []string

	// tsvReader is the underlying reader used to read data in from the TSV
	// or TSV file
	tsvReader *bufio.Reader

	// tsvRecord stores each line of input we read from the underlying reader
	tsvRecord string

	// numProcessed tracks the number of TSV records processed by the underlying reader
	numProcessed uint64

	// numDecoders is the number of concurrent goroutines to use for decoding
	numDecoders int

	// embedded sizeTracker exposes the Size() method to check the number of bytes read so far
	sizeTracker
}

// TSVConverter implements the Converter interface for TSV input.
type TSVConverter struct {
	fields []string
	data   string
	index  uint64
}

// NewTSVInputReader returns a TSVInputReader configured to read input from the
// given io.Reader, extracting the specified fields only.
func NewTSVInputReader(fields []string, in io.Reader, numDecoders int) *TSVInputReader {
	szCount := newSizeTrackingReader(in)
	return &TSVInputReader{
		fields:       fields,
		tsvReader:    bufio.NewReader(in),
		numProcessed: uint64(0),
		numDecoders:  numDecoders,
		sizeTracker:  szCount,
	}
}

// ReadAndValidateHeader reads the header from the underlying reader and validates
// the header fields. It sets err if the read/validation fails.
func (r *TSVInputReader) ReadAndValidateHeader() (err error) {
	header, err := r.tsvReader.ReadString(entryDelimiter)
	if err != nil {
		return err
	}
	for _, field := range strings.Split(header, tokenSeparator) {
		r.fields = append(r.fields, strings.TrimRight(field, "\r\n"))
	}
	return validateReaderFields(r.fields)
}

// StreamDocument takes a boolean indicating if the documents should be streamed
// in read order and a channel on which to stream the documents processed from
// the underlying reader. Returns a non-nil error if streaming fails.
func (r *TSVInputReader) StreamDocument(ordered bool, readDocs chan bson.D) (retErr error) {
	tsvRecordChan := make(chan Converter, r.numDecoders)
	tsvErrChan := make(chan error)

	// begin reading from source
	go func() {
		var err error
		for {
			r.tsvRecord, err = r.tsvReader.ReadString(entryDelimiter)
			if err != nil {
				close(tsvRecordChan)
				if err == io.EOF {
					tsvErrChan <- nil
				} else {
					r.numProcessed++
					tsvErrChan <- fmt.Errorf("read error on entry #%v: %v", r.numProcessed, err)
				}
				return
			}
			tsvRecordChan <- TSVConverter{
				fields: r.fields,
				data:   r.tsvRecord,
				index:  r.numProcessed,
			}
			r.numProcessed++
		}
	}()

	// begin processing read bytes
	go func() {
		tsvErrChan <- streamDocuments(ordered, r.numDecoders, tsvRecordChan, readDocs)
	}()

	return channelQuorumError(tsvErrChan, 2)
}

// Convert implements the Converter interface for TSV input. It converts a
// TSVConverter struct to a BSON document.
func (c TSVConverter) Convert() (bson.D, error) {
	return tokensToBSON(
		c.fields,
		strings.Split(strings.TrimRight(c.data, "\r\n"), tokenSeparator),
		c.index,
	)
}
