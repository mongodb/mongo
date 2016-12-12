// Package bsonutil provides utilities for processing BSON data.
package bsonutil

import (
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"strconv"
	"time"
)

var ErrNoSuchField = errors.New("no such field")

// ConvertJSONDocumentToBSON iterates through the document map and converts JSON
// values to their corresponding BSON values. It also replaces any extended JSON
// type value (e.g. $date) with the corresponding BSON type.
func ConvertJSONDocumentToBSON(doc map[string]interface{}) error {
	for key, jsonValue := range doc {
		var bsonValue interface{}
		var err error

		switch v := jsonValue.(type) {
		case map[string]interface{}, bson.D: // subdocument
			bsonValue, err = ParseSpecialKeys(v)
		default:
			bsonValue, err = ConvertJSONValueToBSON(v)
		}
		if err != nil {
			return err
		}

		doc[key] = bsonValue
	}
	return nil
}

// GetExtendedBsonD iterates through the document and returns a bson.D that adds type
// information for each key in document.
func GetExtendedBsonD(doc bson.D) (bson.D, error) {
	var err error
	var bsonDoc bson.D
	for _, docElem := range doc {
		var bsonValue interface{}
		switch v := docElem.Value.(type) {
		case map[string]interface{}, bson.D: // subdocument
			bsonValue, err = ParseSpecialKeys(v)
		default:
			bsonValue, err = ConvertJSONValueToBSON(v)
		}
		if err != nil {
			return nil, err
		}
		bsonDoc = append(bsonDoc, bson.DocElem{
			Name:  docElem.Name,
			Value: bsonValue,
		})
	}
	return bsonDoc, nil
}

// FindValueByKey returns the value of keyName in document. If keyName is not found
// in the top-level of the document, ErrNoSuchField is returned as the error.
func FindValueByKey(keyName string, document *bson.D) (interface{}, error) {
	for _, key := range *document {
		if key.Name == keyName {
			return key.Value, nil
		}
	}
	return nil, ErrNoSuchField
}

// ParseSpecialKeys takes a JSON document and inspects it for any extended JSON
// type (e.g $numberLong) and replaces any such values with the corresponding
// BSON type.
func ParseSpecialKeys(special interface{}) (interface{}, error) {
	// first ensure we are using a correct document type
	var doc map[string]interface{}
	switch v := special.(type) {
	case bson.D:
		doc = v.Map()
	case map[string]interface{}:
		doc = v
	default:
		return nil, fmt.Errorf("%v (type %T) is not valid input to ParseSpecialKeys", special, special)
	}
	// check document to see if it is special
	switch len(doc) {
	case 1: // document has a single field
		if jsonValue, ok := doc["$date"]; ok {
			switch v := jsonValue.(type) {
			case string:
				return util.FormatDate(v)
			case bson.D:
				asMap := v.Map()
				if jsonValue, ok := asMap["$numberLong"]; ok {
					n, err := parseNumberLongField(jsonValue)
					if err != nil {
						return nil, err
					}
					return time.Unix(n/1e3, n%1e3*1e6), err
				}
				return nil, errors.New("expected $numberLong field in $date")
			case map[string]interface{}:
				if jsonValue, ok := v["$numberLong"]; ok {
					n, err := parseNumberLongField(jsonValue)
					if err != nil {
						return nil, err
					}
					return time.Unix(n/1e3, n%1e3*1e6), err
				}
				return nil, errors.New("expected $numberLong field in $date")

			case json.Number:
				n, err := v.Int64()
				return time.Unix(n/1e3, n%1e3*1e6), err
			case float64:
				n := int64(v)
				return time.Unix(n/1e3, n%1e3*1e6), nil
			case int32:
				n := int64(v)
				return time.Unix(n/1e3, n%1e3*1e6), nil
			case int64:
				return time.Unix(v/1e3, v%1e3*1e6), nil

			case json.ISODate:
				return v, nil

			default:
				return nil, errors.New("invalid type for $date field")
			}
		}

		if jsonValue, ok := doc["$code"]; ok {
			switch v := jsonValue.(type) {
			case string:
				return bson.JavaScript{Code: v}, nil
			default:
				return nil, errors.New("expected $code field to have string value")
			}
		}

		if jsonValue, ok := doc["$oid"]; ok {
			switch v := jsonValue.(type) {
			case string:
				if !bson.IsObjectIdHex(v) {
					return nil, errors.New("expected $oid field to contain 24 hexadecimal character")
				}
				return bson.ObjectIdHex(v), nil

			default:
				return nil, errors.New("expected $oid field to have string value")
			}
		}

		if jsonValue, ok := doc["$numberLong"]; ok {
			return parseNumberLongField(jsonValue)
		}

		if jsonValue, ok := doc["$numberInt"]; ok {
			switch v := jsonValue.(type) {
			case string:
				// all of decimal, hex, and octal are supported here
				n, err := strconv.ParseInt(v, 0, 32)
				return int32(n), err

			default:
				return nil, errors.New("expected $numberInt field to have string value")
			}
		}

		if jsonValue, ok := doc["$timestamp"]; ok {
			ts := json.Timestamp{}

			var tsDoc map[string]interface{}
			switch internalDoc := jsonValue.(type) {
			case map[string]interface{}:
				tsDoc = internalDoc
			case bson.D:
				tsDoc = internalDoc.Map()
			default:
				return nil, errors.New("expected $timestamp key to have internal document")
			}

			if seconds, ok := tsDoc["t"]; ok {
				if asUint32, err := util.ToUInt32(seconds); err == nil {
					ts.Seconds = asUint32
				} else {
					return nil, errors.New("expected $timestamp 't' field to be a numeric type")
				}
			} else {
				return nil, errors.New("expected $timestamp to have 't' field")
			}
			if inc, ok := tsDoc["i"]; ok {
				if asUint32, err := util.ToUInt32(inc); err == nil {
					ts.Increment = asUint32
				} else {
					return nil, errors.New("expected $timestamp 'i' field to be  a numeric type")
				}
			} else {
				return nil, errors.New("expected $timestamp to have 'i' field")
			}
			// see BSON spec for details on the bit fiddling here
			return bson.MongoTimestamp(int64(ts.Seconds)<<32 | int64(ts.Increment)), nil
		}

		if jsonValue, ok := doc["$numberDecimal"]; ok {
			switch v := jsonValue.(type) {
			case string:
				return bson.ParseDecimal128(v)
			default:
				return nil, errors.New("expected $numberDecimal field to have string value")
			}
		}

		if _, ok := doc["$undefined"]; ok {
			return bson.Undefined, nil
		}

		if _, ok := doc["$maxKey"]; ok {
			return bson.MaxKey, nil
		}

		if _, ok := doc["$minKey"]; ok {
			return bson.MinKey, nil
		}

	case 2: // document has two fields
		if jsonValue, ok := doc["$code"]; ok {
			code := bson.JavaScript{}
			switch v := jsonValue.(type) {
			case string:
				code.Code = v
			default:
				return nil, errors.New("expected $code field to have string value")
			}

			if jsonValue, ok = doc["$scope"]; ok {
				switch v2 := jsonValue.(type) {
				case map[string]interface{}, bson.D:
					x, err := ParseSpecialKeys(v2)
					if err != nil {
						return nil, err
					}
					code.Scope = x
					return code, nil
				default:
					return nil, errors.New("expected $scope field to contain map")
				}
			} else {
				return nil, errors.New("expected $scope field with $code field")
			}
		}

		if jsonValue, ok := doc["$regex"]; ok {
			regex := bson.RegEx{}

			switch pattern := jsonValue.(type) {
			case string:
				regex.Pattern = pattern

			default:
				return nil, errors.New("expected $regex field to have string value")
			}
			if jsonValue, ok = doc["$options"]; !ok {
				return nil, errors.New("expected $options field with $regex field")
			}

			switch options := jsonValue.(type) {
			case string:
				regex.Options = options

			default:
				return nil, errors.New("expected $options field to have string value")
			}

			// Validate regular expression options
			for i := range regex.Options {
				switch o := regex.Options[i]; o {
				default:
					return nil, fmt.Errorf("invalid regular expression option '%v'", o)

				case 'g', 'i', 'm', 's': // allowed
				}
			}
			return regex, nil
		}

		if jsonValue, ok := doc["$binary"]; ok {
			binary := bson.Binary{}

			switch data := jsonValue.(type) {
			case string:
				bytes, err := base64.StdEncoding.DecodeString(data)
				if err != nil {
					return nil, err
				}
				binary.Data = bytes

			default:
				return nil, errors.New("expected $binary field to have string value")
			}
			if jsonValue, ok = doc["$type"]; !ok {
				return nil, errors.New("expected $type field with $binary field")
			}

			switch typ := jsonValue.(type) {
			case string:
				kind, err := hex.DecodeString(typ)
				if err != nil {
					return nil, err
				} else if len(kind) != 1 {
					return nil, errors.New("expected single byte (as hexadecimal string) for $type field")
				}
				binary.Kind = kind[0]

			default:
				return nil, errors.New("expected $type field to have string value")
			}
			return binary, nil
		}

		if jsonValue, ok := doc["$ref"]; ok {
			dbRef := mgo.DBRef{}

			switch data := jsonValue.(type) {
			case string:
				dbRef.Collection = data
			default:
				return nil, errors.New("expected string for $ref field")
			}
			if jsonValue, ok = doc["$id"]; ok {
				switch v2 := jsonValue.(type) {
				case map[string]interface{}, bson.D:
					x, err := ParseSpecialKeys(v2)
					if err != nil {
						return nil, fmt.Errorf("error parsing $id field: %v", err)
					}
					dbRef.Id = x
				default:
					dbRef.Id = v2
				}
				return dbRef, nil
			}
		}
	case 3:
		if jsonValue, ok := doc["$ref"]; ok {
			dbRef := mgo.DBRef{}

			switch data := jsonValue.(type) {
			case string:
				dbRef.Collection = data
			default:
				return nil, errors.New("expected string for $ref field")
			}
			if jsonValue, ok = doc["$id"]; ok {
				switch v2 := jsonValue.(type) {
				case map[string]interface{}, bson.D:
					x, err := ParseSpecialKeys(v2)
					if err != nil {
						return nil, fmt.Errorf("error parsing $id field: %v", err)
					}
					dbRef.Id = x
				default:
					dbRef.Id = v2
				}
				if dbValue, ok := doc["$db"]; ok {
					switch v3 := dbValue.(type) {
					case string:
						dbRef.Database = v3
					default:
						return nil, errors.New("expected string for $db field")
					}
					return dbRef, nil
				}
			}
		}
	}

	// nothing matched, so we recurse deeper
	switch v := special.(type) {
	case bson.D:
		return GetExtendedBsonD(v)
	case map[string]interface{}:
		return ConvertJSONValueToBSON(v)
	default:
		return nil, fmt.Errorf("%v (type %T) is not valid input to ParseSpecialKeys", special, special)
	}
}

// ParseJSONValue takes any value generated by the json package and returns a
// BSON version of that value.
func ParseJSONValue(jsonValue interface{}) (interface{}, error) {
	switch v := jsonValue.(type) {
	case map[string]interface{}, bson.D: // subdocument
		return ParseSpecialKeys(v)

	default:
		return ConvertJSONValueToBSON(v)
	}
}

func parseNumberLongField(jsonValue interface{}) (int64, error) {
	switch v := jsonValue.(type) {
	case string:
		// all of decimal, hex, and octal are supported here
		return strconv.ParseInt(v, 0, 64)

	default:
		return 0, errors.New("expected $numberLong field to have string value")
	}
}
