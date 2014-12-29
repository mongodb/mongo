package mongoimport

import (
	"fmt"
	"github.com/mongodb/mongo-tools/mongoimport/csv"
	"gopkg.in/mgo.v2/bson"
	"io"
)

// CSVInputReader is a struct that implements the InputReader interface for a
// CSV input source
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

// CSVConvertibleDoc implements the ConvertibleDoc interface for CSV input
type CSVConvertibleDoc struct {
	fields, data []string
	index        uint64
}

// NewCSVInputReader returns a CSVInputReader configured to read input from the
// given io.Reader, extracting the specified fields only.
func NewCSVInputReader(fields []string, in io.Reader, numDecoders int) *CSVInputReader {
	szCount := &sizeTrackingReader{in, 0}
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

// ReadAndValidateHeader sets the import fields for a CSV importer
func (csvInputReader *CSVInputReader) ReadAndValidateHeader() (err error) {
	fields, err := csvInputReader.csvReader.Read()
	if err != nil {
		return err
	}
	csvInputReader.fields = fields
	return validateReaderFields(csvInputReader.fields)
}

// StreamDocument takes a boolean indicating if the documents should be streamed
// in read order and a channel on which to stream the documents processed from
// the underlying reader. Returns a non-nil error if encountered
func (csvInputReader *CSVInputReader) StreamDocument(ordered bool, readDocChan chan bson.D) (retErr error) {
	csvRecordChan := make(chan ConvertibleDoc, csvInputReader.numDecoders)
	csvErrChan := make(chan error)

	// begin reading from source
	go func() {
		var err error
		for {
			csvInputReader.csvRecord, err = csvInputReader.csvReader.Read()
			if err != nil {
				close(csvRecordChan)
				if err == io.EOF {
					csvErrChan <- nil
				} else {
					csvInputReader.numProcessed++
					csvErrChan <- fmt.Errorf("read error on entry #%v: %v", csvInputReader.numProcessed, err)
				}
				return
			}
			csvRecordChan <- CSVConvertibleDoc{
				fields: csvInputReader.fields,
				data:   csvInputReader.csvRecord,
				index:  csvInputReader.numProcessed,
			}
			csvInputReader.numProcessed++
		}
	}()

	go func() {
		csvErrChan <- streamDocuments(ordered, csvInputReader.numDecoders, csvRecordChan, readDocChan)
	}()

	return channelQuorumError(csvErrChan, 2)
}

// This is required to satisfy the ConvertibleDoc interface for CSV input. It
// does CSV-specific processing to convert the CSVConvertibleDoc to a bson.D
func (csvConvertibleDoc CSVConvertibleDoc) Convert() (bson.D, error) {
	return tokensToBSON(
		csvConvertibleDoc.fields,
		csvConvertibleDoc.data,
		csvConvertibleDoc.index,
	)
}
