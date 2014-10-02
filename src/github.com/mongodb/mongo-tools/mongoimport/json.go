package mongoimport

import (
	"errors"
	"fmt"
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/json"
	"gopkg.in/mgo.v2/bson"
	"io"
	"strings"
)

// JSONImportInput is an implementation of ImportInput that reads documents
// in JSON format.
type JSONImportInput struct {
	// IsArray indicates if the JSON import is an array of JSON documents
	// or not
	IsArray bool
	// Decoder is used to read the next valid JSON documents from the input source
	Decoder *json.Decoder
	// NumProcessed indicates the number of JSON documents processed
	NumProcessed int64
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

// NewJSONImportInput creates a new JSONImportInput in array mode if specified,
// configured to read data to the given io.Reader
func NewJSONImportInput(isArray bool, in io.Reader) *JSONImportInput {
	return &JSONImportInput{
		IsArray:            isArray,
		Decoder:            json.NewDecoder(in),
		readOpeningBracket: false,
		bytesFromReader:    make([]byte, 1),
	}
}

// SetHeader is a no-op for JSON imports
func (jsonImporter *JSONImportInput) SetHeader() error {
	return nil
}

// readJSONArraySeparator is a helper method used to process JSON arrays. It is
// used to read any of the valid separators for a JSON array and flag invalid
// characters.
//
// It will read a byte at a time until it finds an expected character after
// which it returns control to the caller.
//
// TODO: single byte sized scans are inefficient!
//
// It will also return immediately if it finds any error (including EOF). If it
// reads a JSON_ARRAY_END byte, as a validity check it will continue to scan the
// input source until it hits an error (including EOF) to ensure the entire
// input source content is a valid JSON array
func (jsonImporter *JSONImportInput) readJSONArraySeparator() error {
	jsonImporter.expectedByte = JSON_ARRAY_SEP
	if jsonImporter.NumProcessed == 0 {
		jsonImporter.expectedByte = JSON_ARRAY_START
	}

	var readByte byte
	scanp := 0
	jsonImporter.separatorReader = io.MultiReader(jsonImporter.Decoder.Buffered(), jsonImporter.Decoder.R)
	for readByte != jsonImporter.expectedByte {
		n, err := jsonImporter.separatorReader.Read(jsonImporter.bytesFromReader)
		scanp += n
		if n == 0 || err != nil {
			if err == io.EOF {
				return ErrNoClosingBracket
			}
			return err
		}
		readByte = jsonImporter.bytesFromReader[0]

		if readByte == JSON_ARRAY_END {
			// if we read the end of the JSON array, ensure we have no other
			// non-whitespace characters at the end of the array
			for {
				_, err = jsonImporter.separatorReader.Read(jsonImporter.bytesFromReader)
				if err != nil {
					// takes care of the '[]' case
					if !jsonImporter.readOpeningBracket {
						return ErrNoOpeningBracket
					}
					return err
				}
				readString := string(jsonImporter.bytesFromReader[0])
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
			if jsonImporter.expectedByte == JSON_ARRAY_START {
				return ErrNoOpeningBracket
			}
			return fmt.Errorf("bad JSON array format - found '%v' outside "+
				"JSON object/array in input source", string(readByte))
		}
	}
	// adjust the buffer to account for read bytes
	if scanp < len(jsonImporter.Decoder.Buf) {
		jsonImporter.Decoder.Buf = jsonImporter.Decoder.Buf[scanp:]
	} else {
		jsonImporter.Decoder.Buf = []byte{}
	}
	jsonImporter.readOpeningBracket = true
	return nil
}

// ImportDocument converts the given JSON object to a BSON object
func (jsonImporter *JSONImportInput) ImportDocument() (bson.M, error) {
	var document bson.M
	if jsonImporter.IsArray {
		if err := jsonImporter.readJSONArraySeparator(); err != nil {
			if err == io.EOF {
				return nil, err
			}
			jsonImporter.NumProcessed++
			return nil, fmt.Errorf("error reading separator after document #%v: %v", jsonImporter.NumProcessed, err)
		}
	}
	jsonImporter.NumProcessed++
	if err := jsonImporter.Decoder.Decode(&document); err != nil {
		if err == io.EOF {
			return nil, err
		}
		return nil, fmt.Errorf("JSON decode error on document #%v: %v", jsonImporter.NumProcessed, err)
	}

	// convert any data produced by mongoexport to the appropriate underlying
	// extended BSON type. NOTE: this assumes specially formated JSON values
	// in the input JSON - values such as:
	//
	// { $oid: 53cefc71b14ed89d84856287 }
	//
	// should be interpreted as:
	//
	// ObjectId("53cefc71b14ed89d84856287")
	//
	// This applies for all the other extended JSON types MongoDB supports
	if err := bsonutil.ConvertJSONDocumentToBSON(document); err != nil {
		return nil, fmt.Errorf("JSON => BSON conversion error on document #%v: %v", jsonImporter.NumProcessed, err)
	}
	return document, nil
}
