package mongoimport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/mongoimport/csv"
	"gopkg.in/mgo.v2/bson"
	"io"
)

// CSVInputReader implements the InputReader interface for CSV input types.
type CSVInputReader struct {

	// fields is a list of field names in the BSON documents to be imported
	fields []string

	// csvReader is the underlying reader used to read data in from the CSV or CSV file
	csvReader *csv.Reader

	// csvRecord stores each line of input we read from the underlying reader
	csvRecord []string

	// numProcessed tracks the number of CSV records processed by the underlying reader
	numProcessed uint64

	// numDecoders is the number of concurrent goroutines to use for decoding
	numDecoders int

	// embedded sizeTracker exposes the Size() method to check the number of bytes read so far
	sizeTracker
}

// CSVConverter implements the Converter interface for CSV input.
type CSVConverter struct {
	fields, data []string
	index        uint64
}

// NewCSVInputReader returns a CSVInputReader configured to read data from the
// given io.Reader, extracting only the specified fields using exactly "numDecoders"
// goroutines.
func NewCSVInputReader(fields []string, in io.Reader, numDecoders int) *CSVInputReader {
	szCount := newSizeTrackingReader(in)
	csvReader := csv.NewReader(szCount)
	// allow variable number of fields in document
	csvReader.FieldsPerRecord = -1
	csvReader.TrimLeadingSpace = true
	return &CSVInputReader{
		fields:       fields,
		csvReader:    csvReader,
		numProcessed: uint64(0),
		numDecoders:  numDecoders,
		sizeTracker:  szCount,
	}
}

// ReadAndValidateHeader reads the header from the underlying reader and validates
// the header fields. It sets err if the read/validation fails.
func (r *CSVInputReader) ReadAndValidateHeader() (err error) {
	fields, err := r.csvReader.Read()
	if err != nil {
		return err
	}
	r.fields = fields
	return validateReaderFields(r.fields)
}

// StreamDocument takes a boolean indicating if the documents should be streamed
// in read order and a channel on which to stream the documents processed from
// the underlying reader. Returns a non-nil error if streaming fails.
func (r *CSVInputReader) StreamDocument(ordered bool, readDocs chan bson.D) (retErr error) {
	csvRecordChan := make(chan Converter, r.numDecoders)
	csvErrChan := make(chan error)

	// begin reading from source
	go func() {
		var err error
		for {
			r.csvRecord, err = r.csvReader.Read()
			if err != nil {
				close(csvRecordChan)
				if err == io.EOF {
					csvErrChan <- nil
				} else {
					r.numProcessed++
					csvErrChan <- fmt.Errorf("read error on entry #%v: %v", r.numProcessed, err)
				}
				return
			}
			csvRecordChan <- CSVConverter{
				fields: r.fields,
				data:   r.csvRecord,
				index:  r.numProcessed,
			}
			r.numProcessed++
		}
	}()

	go func() {
		csvErrChan <- streamDocuments(ordered, r.numDecoders, csvRecordChan, readDocs)
	}()

	return channelQuorumError(csvErrChan, 2)
}

// Convert implements the Converter interface for CSV input. It converts a
// CSVConverter struct to a BSON document.
func (c CSVConverter) Convert() (bson.D, error) {
	return tokensToBSON(
		c.fields,
		c.data,
		c.index,
	)
}
