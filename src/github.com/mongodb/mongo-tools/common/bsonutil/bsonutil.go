package bsonutil

import (
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"github.com/mongodb/mongo-tools/common/json"
	"gopkg.in/mgo.v2/bson"
	"strconv"
	"time"
)

var (
	acceptedDateFormats = []string{
		"2006-01-02T15:04:05.000Z0700",
		"2006-01-02T15:04:05Z0700",
		"2006-01-02T15:04Z0700",
	}
)

func ConvertJSONDocumentToBSON(doc map[string]interface{}) error {
	for key, jsonValue := range doc {
		var bsonValue interface{}
		var err error

		switch v := jsonValue.(type) {
		case map[string]interface{}: // subdocument
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

// ParseSpecialKeys takes a JSON document and inspects it for any
// special ('$') keys and replaces any values with the corresponding
// BSON type.
func ParseSpecialKeys(doc map[string]interface{}) (interface{}, error) {
	switch len(doc) {
	case 1: // document has a single field
		if jsonValue, ok := doc["$date"]; ok {
			var date time.Time
			var err error

			switch v := jsonValue.(type) {
			case string:
				for _, format := range acceptedDateFormats {
					date, err = time.Parse(format, v)
					if err == nil {
						return date, nil
					}
				}
				return date, err

			case map[string]interface{}:
				if jsonValue, ok := v["$numberLong"]; ok {
					n, err := parseNumberLongField(jsonValue)
					if err != nil {
						return nil, err
					}
					return time.Unix(n/1e3, n%1e3*1e6), err
				}
				return nil, errors.New("Expected $numberLong field in $date")

			case json.Number:
				n, err := v.Int64()
				return time.Unix(n/1e3, n%1e3*1e6), err

			case float64:
				n := int64(v)
				return time.Unix(n/1e3, n%1e3*1e6), nil

			case int64:
				return time.Unix(v/1e3, v%1e3*1e6), nil

			default:
				return nil, errors.New("Invalid type for $date field")
			}
		}

		if jsonValue, ok := doc["$oid"]; ok {
			switch v := jsonValue.(type) {
			case string:
				if !bson.IsObjectIdHex(v) {
					return nil, errors.New("Expected $oid field to contain 24 hexadecimal character")
				}
				return bson.ObjectIdHex(v), nil

			default:
				return nil, errors.New("Expected $oid field to have string value")
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
				return nil, errors.New("Expected $numberInt field to have string value")
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
		if jsonValue, ok := doc["$regex"]; ok {
			regex := bson.RegEx{}

			switch pattern := jsonValue.(type) {
			case string:
				regex.Pattern = pattern

			default:
				return nil, errors.New("Expected $regex field to have string value")
			}
			if jsonValue, ok = doc["$options"]; !ok {
				return nil, errors.New("Expected $options field with $regex field")
			}

			switch options := jsonValue.(type) {
			case string:
				regex.Options = options

			default:
				return nil, errors.New("Expected $options field to have string value")
			}

			// Validate regular expression options
			for i := range regex.Options {
				switch o := regex.Options[i]; o {
				default:
					return nil, fmt.Errorf("Invalid regular expression option '%v'", o)

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
				return nil, errors.New("Expected $binary field to have string value")
			}
			if jsonValue, ok = doc["$type"]; !ok {
				return nil, errors.New("Expected $type field with $binary field")
			}

			switch typ := jsonValue.(type) {
			case string:
				kind, err := hex.DecodeString(typ)
				if err != nil {
					return nil, err
				} else if len(kind) != 1 {
					return nil, errors.New("Expected single byte (as hexadecimal string) for $type field")
				}
				binary.Kind = kind[0]

			default:
				return nil, errors.New("Expected $type field to have string value")
			}
			return binary, nil
		}
	}

	// Did not match any special ('$') keys, so convert all sub-values.
	return ConvertJSONValueToBSON(doc)
}

func parseJSONValue(jsonValue interface{}) (interface{}, error) {
	switch v := jsonValue.(type) {
	case map[string]interface{}: // subdocument
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
		return 0, errors.New("Expected $numberLong field to have string value")
	}
}
