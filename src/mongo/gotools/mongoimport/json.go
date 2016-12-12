package mongoimport

import (
	"errors"
	"fmt"
	"io"
	"strings"

	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/log"
	"gopkg.in/mgo.v2/bson"
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

// JSONConverter implements the Converter interface for JSON input.
type JSONConverter struct {
	data  []byte
	index uint64
}

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
// configured to read data to the given io.Reader.
func NewJSONInputReader(isArray bool, in io.Reader, numDecoders int) *JSONInputReader {
	szCount := newSizeTrackingReader(newBomDiscardingReader(in))
	return &JSONInputReader{
		isArray:            isArray,
		sizeTracker:        szCount,
		decoder:            json.NewDecoder(szCount),
		readOpeningBracket: false,
		bytesFromReader:    make([]byte, 1),
		numDecoders:        numDecoders,
	}
}

// ReadAndValidateHeader is a no-op for JSON imports; always returns nil.
func (r *JSONInputReader) ReadAndValidateHeader() error {
	return nil
}

// ReadAndValidateTypedHeader is a no-op for JSON imports; always returns nil.
func (r *JSONInputReader) ReadAndValidateTypedHeader(parseGrace ParseGrace) error {
	return nil
}

// StreamDocument takes a boolean indicating if the documents should be streamed
// in read order and a channel on which to stream the documents processed from
// the underlying reader. Returns a non-nil error if encountered
func (r *JSONInputReader) StreamDocument(ordered bool, readChan chan bson.D) (retErr error) {
	rawChan := make(chan Converter, r.numDecoders)
	jsonErrChan := make(chan error)

	// begin reading from source
	go func() {
		var err error
		for {
			if r.isArray {
				if err = r.readJSONArraySeparator(); err != nil {
					close(rawChan)
					if err == io.EOF {
						jsonErrChan <- nil
					} else {
						r.numProcessed++
						jsonErrChan <- fmt.Errorf("error reading separator after document #%v: %v", r.numProcessed, err)
					}
					return
				}
			}
			rawBytes, err := r.decoder.ScanObject()
			if err != nil {
				close(rawChan)
				if err == io.EOF {
					jsonErrChan <- nil
				} else {
					r.numProcessed++
					jsonErrChan <- fmt.Errorf("error processing document #%v: %v", r.numProcessed, err)
				}
				return
			}
			rawChan <- JSONConverter{
				data:  rawBytes,
				index: r.numProcessed,
			}
			r.numProcessed++
		}
	}()

	// begin processing read bytes
	go func() {
		jsonErrChan <- streamDocuments(ordered, r.numDecoders, rawChan, readChan)
	}()

	return channelQuorumError(jsonErrChan, 2)
}

// Convert implements the Converter interface for JSON input. It converts a
// JSONConverter struct to a BSON document.
func (c JSONConverter) Convert() (bson.D, error) {
	document, err := json.UnmarshalBsonD(c.data)
	if err != nil {
		return nil, fmt.Errorf("error unmarshaling bytes on document #%v: %v", c.index, err)
	}
	log.Logvf(log.DebugHigh, "got line: %v", document)

	bsonD, err := bsonutil.GetExtendedBsonD(document)
	if err != nil {
		return nil, fmt.Errorf("error getting extended BSON for document #%v: %v", c.index, err)
	}
	log.Logvf(log.DebugHigh, "got extended line: %#v", bsonD)
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
func (r *JSONInputReader) readJSONArraySeparator() error {
	r.expectedByte = json.ArraySep
	if r.numProcessed == 0 {
		r.expectedByte = json.ArrayStart
	}

	var readByte byte
	scanp := 0

	separatorReader := io.MultiReader(
		r.decoder.Buffered(),
		r.decoder.R,
	)
	for readByte != r.expectedByte {
		n, err := separatorReader.Read(r.bytesFromReader)
		scanp += n
		if n == 0 || err != nil {
			if err == io.EOF {
				return ErrNoClosingBracket
			}
			return err
		}
		readByte = r.bytesFromReader[0]

		if readByte == json.ArrayEnd {
			// if we read the end of the JSON array, ensure we have no other
			// non-whitespace characters at the end of the array
			for {
				_, err = separatorReader.Read(r.bytesFromReader)
				if err != nil {
					// takes care of the '[]' case
					if !r.readOpeningBracket {
						return ErrNoOpeningBracket
					}
					return err
				}
				readString := string(r.bytesFromReader[0])
				if strings.TrimSpace(readString) != "" {
					return fmt.Errorf("bad JSON array format - found '%v' "+
						"after '%v' in input source", readString,
						string(json.ArrayEnd))
				}
			}
		}

		// this will catch any invalid inter JSON object byte that occurs in the
		// input source
		if !(readByte == json.ArraySep ||
			strings.TrimSpace(string(readByte)) == "" ||
			readByte == json.ArrayStart ||
			readByte == json.ArrayEnd) {
			if r.expectedByte == json.ArrayStart {
				return ErrNoOpeningBracket
			}
			return fmt.Errorf("bad JSON array format - found '%v' outside "+
				"JSON object/array in input source", string(readByte))
		}
	}
	// adjust the buffer to account for read bytes
	if scanp < len(r.decoder.Buf) {
		r.decoder.Buf = r.decoder.Buf[scanp:]
	} else {
		r.decoder.Buf = []byte{}
	}
	r.readOpeningBracket = true
	return nil
}
