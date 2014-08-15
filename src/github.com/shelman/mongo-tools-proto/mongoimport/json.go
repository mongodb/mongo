package mongoimport

import (
	"encoding/json"
	"errors"
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/bson_ext"
	"gopkg.in/mgo.v2/bson"
	"io"
	"strings"
)

// JSONImportInput is an implementation of ImportInput that reads documents
// in JSON format.
type JSONImportInput struct {
	// IsArray indicates if the JSON import is an array of JSON objects (true)
	// or not
	IsArray bool
	// Decoder is used to read the next valid JSON object from the input source
	Decoder *json.Decoder
	// Reader is used to advance the underlying io.Reader implementation to skip
	// things like array start/end and commas
	Reader io.Reader
	// NumImported indicates the number of JSON objects successfully parsed from
	// the JSON input source
	NumImported int64
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
		Reader:             in,
		NumImported:        0,
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
	if jsonImporter.NumImported == 0 {
		jsonImporter.expectedByte = JSON_ARRAY_START
	}

	var readByte byte
	for readByte != jsonImporter.expectedByte {
		n, err := jsonImporter.Reader.Read(jsonImporter.bytesFromReader)
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
				_, err = jsonImporter.Reader.Read(jsonImporter.bytesFromReader)
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
			readByte == ' ' ||
			readByte == JSON_ARRAY_START ||
			readByte == JSON_ARRAY_END) {
			if jsonImporter.expectedByte == JSON_ARRAY_START {
				return ErrNoOpeningBracket
			}
			return fmt.Errorf("bad JSON array format - found '%v' outside "+
				"JSON object/array in input source", string(readByte))
		}
	}
	jsonImporter.readOpeningBracket = true
	return nil
}

// ImportDocument converts the given JSON object to a BSON object
func (jsonImporter *JSONImportInput) ImportDocument() (bson.M, error) {
	var document bson.M
	if jsonImporter.IsArray {
		if err := jsonImporter.readJSONArraySeparator(); err != nil {
			return nil, err
		}
	}

	if err := jsonImporter.Decoder.Decode(&document); err != nil {
		return nil, err
	}

	// reinitialize the reader with data left in the decoder's buffer and the
	// handle to the underlying reader
	//
	// TODO: this builds a massive tree - depending on the size of the input
	// source; probably some room for optimization here?
	jsonImporter.Reader = io.MultiReader(jsonImporter.Decoder.Buffered(),
		jsonImporter.Reader)

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
	if err := bson_ext.ConvertSubdocsFromJSON(document); err != nil {
		return nil, err
	}
	jsonImporter.NumImported++

	// reinitialize the decoder with its existing buffer and the underlying
	// reader
	jsonImporter.Decoder = json.NewDecoder(jsonImporter.Reader)
	return document, nil
}
