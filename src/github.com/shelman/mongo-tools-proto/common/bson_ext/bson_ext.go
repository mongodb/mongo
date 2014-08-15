package bson_ext

import (
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"reflect"
	"strconv"
	"time"
)

type BSONExt map[string]interface{}
type ObjectIdExt bson.ObjectId

//TODO NumberIntExt, null
type NumberLongExt int64
type MongoTimestampExt bson.MongoTimestamp
type GenericBinaryExt []byte
type BinaryExt bson.Binary
type JavascriptExt bson.JavaScript
type TimeExt time.Time
type RegExExt bson.RegEx
type MinKeyExt struct{}
type MaxKeyExt struct{}
type UndefinedExt struct{}
type DBRefExt mgo.DBRef

var (
	acceptedDateFormats = []string{
		"Mon Jan 2 2006 15:04:05 MST-0700 (EDT)",
		"2006-01-02 15:04:05.00 -0700 EDT",
		"2006-01-02 15:04:05 -0700 EDT",
		"2006-01-02 15:04:05 -0700 EST",
		"2006-01-02T15:04:05.000-0700",
	}
)

/* ObjectID */
func (m ObjectIdExt) MarshalJSON() ([]byte, error) {
	return []byte("{\"$oid\":\"" + bson.ObjectId(m).Hex() + "\"}"), nil
}

func (m ObjectIdExt) String() string {
	return "ObjectId(\"" + bson.ObjectId(m).Hex() + "\")"
}

/* NumberLong */
func (m NumberLongExt) MarshalJSON() ([]byte, error) {
	return []byte(fmt.Sprintf("{\"$numberLong\":\"%v\"}", m)), nil
}

func (m NumberLongExt) String() string {
	return fmt.Sprintf("%v", int64(m))
}

/* Timestamp */
func (m MongoTimestampExt) MarshalJSON() ([]byte, error) {
	inc := int64(m) & 0xFFFF
	ts := (int64(m) & (0xFFFF << 32)) >> 32
	return []byte(fmt.Sprintf("{\"$timestamp\":{\"t\": %v, \"i\":%v}}", ts, inc)), nil
}

func (m MongoTimestampExt) String() string {
	inc := int64(m) & 0xFFFF
	ts := (int64(m) & (0xFFFF << 32)) >> 32
	return fmt.Sprintf("{\"$timestamp\":{\"t\": %v, \"i\":%v}}", inc, ts)

	//return ""
}

/* Binary */

func (m GenericBinaryExt) MarshalJSON() ([]byte, error) {
	return []byte(fmt.Sprintf("{\"$binary\":\"%v\", \"$type\":\"00\"}",
		base64.StdEncoding.EncodeToString(m))), nil
}

func (m BinaryExt) String() string {
	return hex.EncodeToString(m.Data)
}

func (m GenericBinaryExt) String() string {
	return hex.EncodeToString(m)
}

/* MinKey / MaxKey */
func (m MinKeyExt) MarshalJSON() ([]byte, error) {
	return []byte(fmt.Sprintf("{\"$minKey\":1}")), nil
}

func (m MinKeyExt) String() string {
	return "$MinKey"
}

func (m MaxKeyExt) MarshalJSON() ([]byte, error) {
	return []byte(fmt.Sprintf("{\"$maxKey\":1}")), nil
}

func (m MaxKeyExt) String() string {
	return "$MaxKey"
}

func (m UndefinedExt) MarshalJSON() ([]byte, error) {
	return []byte(fmt.Sprintf("{\"$undefined\":true}")), nil
}

func (m UndefinedExt) String() string {
	return ""
}

/* Date/Time */

func (m TimeExt) MarshalJSON() ([]byte, error) {
	return []byte(fmt.Sprintf("{\"$date\":\"%v\"}", m)), nil
}

func (m TimeExt) String() string {
	return fmt.Sprintf("%s", time.Time(m))
}

func (m RegExExt) MarshalJSON() ([]byte, error) {
	return []byte(fmt.Sprintf("{\"$regex\":\"%v\",\"$options\":\"%v\"}", m.Pattern, m.Options)), nil
}

func (m JavascriptExt) MarshalJSON() ([]byte, error) {
	return json.Marshal(m.Code)
	//return []byte("\"" + m.Code + "\""), nil
}

func ConvertSubdocsFromJSON(doc map[string]interface{}) error {
	for key, val := range doc {
		if valSubDoc, ok := val.(map[string]interface{}); ok {
			newVal, err := ParseExtendedJSON(valSubDoc)
			if err != nil {
				return err
			}
			doc[key] = newVal
		}
	}
	return nil
}

//ParseExtendedJSON takes a basic map (from unmarshalled JSON) and inspect it
//for any special extended-json structures, replacing them with the correct
//BSON types where necessary.
func ParseExtendedJSON(doc map[string]interface{}) (interface{}, error) {
	docSize := len(doc)
	// extended JSON representations with only 1 field
	if docSize == 1 {
		if dateValue, ok := doc["$date"]; ok {
			if dateStr, isStr := dateValue.(string); isStr {
				// "Mon Jun 30 2014 16:50:34 GMT-0400 (EDT)"
				// TODO: add other parse formats
				var date time.Time
				var err error
				for _, format := range acceptedDateFormats {
					date, err = time.Parse(format, dateStr)
					if err != nil {
						continue
					}
					break
				}
				return date, err
			}
			return nil, fmt.Errorf("Invalid date string")
		}

		if oidValue, ok := doc["$oid"]; ok {
			if oidStr, isStr := oidValue.(string); isStr {
				return bson.ObjectIdHex(oidStr), nil
			}
			return nil, fmt.Errorf("Value is not a valid ObjectId hex string: %v", oidValue)
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

		if val, ok := doc["$numberLong"]; ok {
			if valStr, isStr := val.(string); isStr {
				intVal, err := strconv.ParseInt(valStr, 10, 64)
				if err != nil {
					return nil, err
				}
				return NumberLongExt(intVal), nil
			}
			return nil, fmt.Errorf("Invalid numberLong string.")
		}

		err := ConvertSubdocsFromJSON(doc)
		if err != nil {
			return nil, err
		}
		return doc, nil
	} else if docSize == 2 {
		if pattern, hasPattern := doc["$regex"]; hasPattern {
			if options, hasOptions := doc["$options"]; hasOptions {
				patternStr, patternIsStr := pattern.(string)
				optionsStr, optionsIsStr := options.(string)
				if !(patternIsStr && optionsIsStr) {
					return nil, fmt.Errorf("Invalid regex")
				}
				return bson.RegEx{
					Pattern: patternStr,
					Options: optionsStr,
				}, nil

			}
		}

		if binary, hasBinary := doc["$binary"]; hasBinary {
			if binType, hasType := doc["$type"]; hasType {
				binaryStr, binIsStr := binary.(string)
				typeStr, typeIsStr := binType.(string)
				if !(binIsStr && typeIsStr) {
					return nil, fmt.Errorf("Invalid binary data")
				}
				data, err := base64.StdEncoding.DecodeString(binaryStr)
				if err != nil {
					return nil, fmt.Errorf("Invalid base64 in binary string: %v", err)
				}
				binTypeByte, err := hex.DecodeString(typeStr)
				if err != nil {
					return nil, fmt.Errorf("Invalid type for BinData: %v", err)
				}
				if len(binTypeByte) != 1 {
					return nil, fmt.Errorf("Invalid type for BinData: %v", err)
				}
				return bson.Binary{Kind: binTypeByte[0], Data: data}, nil
			}
		}
		err := ConvertSubdocsFromJSON(doc)
		if err != nil {
			return nil, err
		}
		return doc, nil
	} else {
		err := ConvertSubdocsFromJSON(doc)
		if err != nil {
			return nil, err
		}
		return doc, nil
	}
}

//GetExtendedBSON walks through a document and replaces any special BSON
//types with equivalent types that support formatting as extended JSON.
func GetExtendedBSON(value interface{}) interface{} {
	switch t := value.(type) {
	case bson.M:
		for key, val := range t {
			t[key] = GetExtendedBSON(val)
		}
		return t
	case []interface{}:
		for index, val := range t {
			t[index] = GetExtendedBSON(val)
		}
		return t
	case int64:
		return NumberLongExt(t)
	case bson.ObjectId:
		return ObjectIdExt(t)
	case bson.MongoTimestamp:
		return MongoTimestampExt(t)
	case []byte:
		return GenericBinaryExt(t)
	case bson.Binary:
		return BinaryExt(t)
	case bson.JavaScript:
		return JavascriptExt(t)
	case time.Time:
		return TimeExt(t)
	case bson.RegEx:
		return RegExExt(t)
	default:
		valueReflect := reflect.ValueOf(value)
		if valueReflect == reflect.ValueOf(bson.MinKey) {
			return MinKeyExt{}
		} else if valueReflect == reflect.ValueOf(bson.MaxKey) {
			return MaxKeyExt{}
		} else if valueReflect == reflect.ValueOf(bson.Undefined) {
			return UndefinedExt{}
		}
		return t
	}
	return value
}

/*
func (m DBRefExt) MarshalJSON() ([]byte, error) {
	return
	//return []byte("{\"$ref\":\"dkgdgsg" + m.Database + "." + m.Collection + "\"}"), nil
}

func (m DBRefExt) String() string {
	return ""
}
*/
