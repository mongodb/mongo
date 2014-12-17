package mongoimport

import (
	"errors"
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/log"
	"gopkg.in/mgo.v2/bson"
	"io"
	"strings"
)

// JSONInputReader is an implementation of InputReader that reads documents
// in JSON format.
type JSONInputReader struct {
	// isArray indicates if the JSON import is an array of JSON documents
	// or not
	isArray bool

	// decoder is used to read the 	next valid JSON documents from the input source
	decoder *json.Decoder

	// numProcessed indicates the number of JSON documents processed
	numProcessed uint64

	// readOpeningBracket indicates if the underlying io.Reader has consumed
	// an opening bracket from the input source. Used to prevent errors when
	// a JSON input source contains just '[]'
	readOpeningBracket bool

	// expectedByte is used to store the next expected valid character for JSON
	// array imports
	expectedByte byte

	// bytesFromReader is used to store the next byte read from the Reader for
	// JSON array imports
	bytesFromReader []byte

	// separatorReader is used for JSON arrays to look for a valid array
	// separator. It is a reader consisting of the decoder's buffer and the
	// underlying reader
	separatorReader io.Reader

	// embedded sizeTracker exposes the Size() method to check the number of bytes read so far
	sizeTracker

	// numDecoders is the number of concurrent goroutines to use for decoding
	numDecoders int
}

// JSONConvertibleDoc implements the ConvertibleDoc interface for JSON input
type JSONConvertibleDoc struct {
	data  []byte
	index uint64
}

const (
	JSON_ARRAY_START = '['
	JSON_ARRAY_SEP   = ','
	JSON_ARRAY_END   = ']'
)

var (
	// ErrNoOpeningBracket means that the input source did not contain any
	// opening brace - returned only if --jsonArray is passed in.
	ErrNoOpeningBracket = errors.New("bad JSON array format - found no " +
		"opening bracket '[' in input source")

	// ErrNoClosingBracket means that the input source did not contain any
	// closing brace - returned only if --jsonArray is passed in.
	ErrNoClosingBracket = errors.New("bad JSON array format - found no " +
		"closing bracket ']' in input source")
)

// NewJSONInputReader creates a new JSONInputReader in array mode if specified,
// configured to read data to the given io.Reader
func NewJSONInputReader(isArray bool, in io.Reader, numDecoders int) *JSONInputReader {
	szCount := &sizeTrackingReader{in, 0}
	return &JSONInputReader{
		isArray:            isArray,
		sizeTracker:        szCount,
		decoder:            json.NewDecoder(szCount),
		readOpeningBracket: false,
		bytesFromReader:    make([]byte, 1),
		numDecoders:        numDecoders,
	}
}

// ReadAndValidateHeader is a no-op for JSON imports
func (jsonInputReader *JSONInputReader) ReadAndValidateHeader() error {
	return nil
}

// StreamDocument takes in two channels: it sends processed documents on the
// readChan channel and if any error is encountered, the error is sent on the
// errChan channel. It keeps reading from the underlying input source until it
// hits EOF or an error. If ordered is true, it streams the documents in which
// the documents are read
func (jsonInputReader *JSONInputReader) StreamDocument(ordered bool, readChan chan bson.D, errChan chan error) {
	rawChan := make(chan ConvertibleDoc, jsonInputReader.numDecoders)
	go func() {
		var err error
		for {
			if jsonInputReader.isArray {
				if err = jsonInputReader.readJSONArraySeparator(); err != nil {
					if err != io.EOF {
						jsonInputReader.numProcessed++
						errChan <- fmt.Errorf("error reading separator after document #%v: %v", jsonInputReader.numProcessed, err)
					}
					close(rawChan)
					return
				}
			}
			rawBytes, err := jsonInputReader.decoder.ScanObject()
			if err != nil {
				if err != io.EOF {
					jsonInputReader.numProcessed++
					errChan <- fmt.Errorf("error processing document #%v: %v", jsonInputReader.numProcessed, err)
				}
				close(rawChan)
				return
			}
			rawChan <- JSONConvertibleDoc{
				data:  rawBytes,
				index: jsonInputReader.numProcessed,
			}
			jsonInputReader.numProcessed++
		}
	}()
	errChan <- streamDocuments(ordered, jsonInputReader.numDecoders, rawChan, readChan)
}

// This is required to satisfy the ConvertibleDoc interface for JSON input. It
// does JSON-specific processing to convert the JSONConvertibleDoc to a bson.D
func (jsonConvertibleDoc JSONConvertibleDoc) Convert() (bson.D, error) {
	document, err := json.UnmarshalBsonD(jsonConvertibleDoc.data)
	if err != nil {
		return nil, fmt.Errorf("error unmarshaling bytes on document #%v: %v", jsonConvertibleDoc.index, err)
	}
	log.Logf(log.DebugHigh, "got line: %v", document)

	bsonD, err := bsonutil.GetExtendedBsonD(document)
	if err != nil {
		return nil, fmt.Errorf("error getting extended BSON for document #%v: %v", jsonConvertibleDoc.index, err)
	}
	log.Logf(log.DebugHigh, "got extended line: %#v", bsonD)
	return bsonD, nil
}

// readJSONArraySeparator is a helper method used to process JSON arrays. It is
// used to read any of the valid separators for a JSON array and flag invalid
// characters.
//
// It will read a byte at a time until it finds an expected character after
// which it returns control to the caller.
//
// It will also return immediately if it finds any error (including EOF). If it
// reads a JSON_ARRAY_END byte, as a validity check it will continue to scan the
// input source until it hits an error (including EOF) to ensure the entire
// input source content is a valid JSON array
func (jsonInputReader *JSONInputReader) readJSONArraySeparator() error {
	jsonInputReader.expectedByte = JSON_ARRAY_SEP
	if jsonInputReader.numProcessed == 0 {
		jsonInputReader.expectedByte = JSON_ARRAY_START
	}

	var readByte byte
	scanp := 0

	separatorReader := io.MultiReader(
		jsonInputReader.decoder.Buffered(),
		jsonInputReader.decoder.R,
	)
	for readByte != jsonInputReader.expectedByte {
		n, err := separatorReader.Read(jsonInputReader.bytesFromReader)
		scanp += n
		if n == 0 || err != nil {
			if err == io.EOF {
				return ErrNoClosingBracket
			}
			return err
		}
		readByte = jsonInputReader.bytesFromReader[0]

		if readByte == JSON_ARRAY_END {
			// if we read the end of the JSON array, ensure we have no other
			// non-whitespace characters at the end of the array
			for {
				_, err = separatorReader.Read(jsonInputReader.bytesFromReader)
				if err != nil {
					// takes care of the '[]' case
					if !jsonInputReader.readOpeningBracket {
						return ErrNoOpeningBracket
					}
					return err
				}
				readString := string(jsonInputReader.bytesFromReader[0])
				if strings.TrimSpace(readString) != "" {
					return fmt.Errorf("bad JSON array format - found '%v' "+
						"after '%v' in input source", readString,
						string(JSON_ARRAY_END))
				}
			}
		}

		// this will catch any invalid inter JSON object byte that occurs in the
		// input source
		if !(readByte == JSON_ARRAY_SEP ||
			strings.TrimSpace(string(readByte)) == "" ||
			readByte == JSON_ARRAY_START ||
			readByte == JSON_ARRAY_END) {
			if jsonInputReader.expectedByte == JSON_ARRAY_START {
				return ErrNoOpeningBracket
			}
			return fmt.Errorf("bad JSON array format - found '%v' outside "+
				"JSON object/array in input source", string(readByte))
		}
	}
	// adjust the buffer to account for read bytes
	if scanp < len(jsonInputReader.decoder.Buf) {
		jsonInputReader.decoder.Buf = jsonInputReader.decoder.Buf[scanp:]
	} else {
		jsonInputReader.decoder.Buf = []byte{}
	}
	jsonInputReader.readOpeningBracket = true
	return nil
}
