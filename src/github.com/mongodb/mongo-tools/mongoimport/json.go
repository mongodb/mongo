package mongoimport

import (
	"github.com/mongodb/mongo-tools/common/bsonutil"
	"github.com/mongodb/mongo-tools/common/json"
	"gopkg.in/mgo.v2/bson"
	"io"
)

// JSONImportInput is an implementation of ImportInput that reads documents
// in JSON format.
type JSONImportInput struct {
	// IsArray indicates if the JSON import is an array of JSON objects (true)
	// or not
	IsArray bool
	// Decoder is used to read the next valid JSON object from the input source
	Decoder *json.Decoder
	// NumImported indicates the number of JSON objects successfully parsed from
	// the JSON input source
	NumImported int64
}

// NewJSONImportInput creates a new JSONImportInput in array mode if specified,
// configured to read data to the given io.Reader
func NewJSONImportInput(isArray bool, in io.Reader) *JSONImportInput {
	var decoder *json.Decoder
	if isArray {
		decoder = json.NewTopLevelArrayDecoder(in)
	} else {
		decoder = json.NewDecoder(in)
	}
	return &JSONImportInput{
		IsArray: isArray,
		Decoder: decoder,
	}
}

// SetHeader is a no-op for JSON imports
func (jsonImporter *JSONImportInput) SetHeader() error {
	return nil
}

// ImportDocument converts the given JSON object to a BSON object
func (jsonImporter *JSONImportInput) ImportDocument() (document bson.M, err error) {
	if err = jsonImporter.Decoder.Decode(&document); err != nil {
		return nil, err
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
		return nil, err
	}
	jsonImporter.NumImported++
	return document, nil
}
