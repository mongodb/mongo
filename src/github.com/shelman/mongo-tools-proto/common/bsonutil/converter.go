package bsonutil

import (
	"encoding/base64"
	"errors"
	"fmt"
	"github.com/shelman/mongo-tools-proto/common/json"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
	"reflect"
	"time"
)

// ConvertJSONValueToBSON walks through a document or an array and
// replaces any extended JSON value with its corresponding BSON type.
func ConvertJSONValueToBSON(x interface{}) (interface{}, error) {
	switch v := x.(type) {
	case nil:
		return nil, nil
	case bool:
		return v, nil
	case map[string]interface{}: // document
		for key, jsonValue := range v {
			bsonValue, err := parseJSONValue(jsonValue)
			if err != nil {
				return nil, err
			}
			v[key] = bsonValue
		}
		return v, nil

	case []interface{}: // array
		for i, jsonValue := range v {
			bsonValue, err := parseJSONValue(jsonValue)
			if err != nil {
				return nil, err
			}
			v[i] = bsonValue
		}
		return v, nil

	case string, float64:
		return v, nil // require no conversion

	case json.ObjectId: // ObjectId
		s := string(v)
		if !bson.IsObjectIdHex(s) {
			return nil, errors.New("Expected ObjectId to contain 24 hexadecimal characters")
		}
		return bson.ObjectIdHex(s), nil

	case json.Date: // Date
		n := int64(v)
		return time.Unix(n/1e3, n%1e3*1e6), nil

	case json.NumberLong: // NumberLong
		return int64(v), nil

	case json.NumberInt: // NumberInt
		return int32(v), nil

	case json.BinData: // BinData
		data, err := base64.StdEncoding.DecodeString(v.Base64)
		if err != nil {
			return nil, err
		}
		return bson.Binary{v.Type, data}, nil

	case json.DBRef: // DBRef
		return mgo.DBRef{v.Collection, v.Id, v.Database}, nil

	case json.RegExp: // RegExp
		return bson.RegEx{v.Pattern, v.Options}, nil

	case json.Timestamp: // Timestamp
		ts := (int64(v.Seconds) << 32) | int64(v.Increment)
		return bson.MongoTimestamp(ts), nil

	case json.MinKey: // MinKey
		return bson.MinKey, nil

	case json.MaxKey: // MaxKey
		return bson.MaxKey, nil

	case json.Undefined: // undefined
		return bson.Undefined, nil
	default:
		return nil, fmt.Errorf("Conversion of JSON type '%v' unsupported", v)
	}

}

// ConvertBSONValueToJSON walks through a document or an array and
// replaces any BSON value with its corresponding extended JSON type.
func ConvertBSONValueToJSON(x interface{}) (interface{}, error) {
	switch v := x.(type) {
	case nil:
		return nil, nil
	case bool:
		return v, nil
	case *bson.M: // document
		v2 := *v
		for key, value := range v2 {
			jsonValue, err := ConvertBSONValueToJSON(value)
			if err != nil {
				return nil, err
			}
			v2[key] = jsonValue
		}
		return v, nil
	case bson.M: // document
		for key, value := range v {
			jsonValue, err := ConvertBSONValueToJSON(value)
			if err != nil {
				return nil, err
			}
			v[key] = jsonValue
		}
		return v, nil

	case []interface{}: // array
		for i, value := range v {
			jsonValue, err := ConvertBSONValueToJSON(value)
			if err != nil {
				return nil, err
			}
			v[i] = jsonValue
		}
		return v, nil

	case string, float64:
		return v, nil // require no conversion

	case int:
		return json.NumberInt(v), nil

	case bson.ObjectId: // ObjectId
		return json.ObjectId(v.Hex()), nil

	case time.Time: // Date
		return json.Date(v.Unix()*1000 + int64(v.Nanosecond()/1e6)), nil

	case int64: // NumberLong
		return json.NumberLong(v), nil

	case int32: // NumberInt
		return json.NumberInt(v), nil

	case []byte: // BinData (with generic type)
		data := base64.StdEncoding.EncodeToString(v)
		return json.BinData{0x00, data}, nil

	case bson.Binary: // BinData
		data := base64.StdEncoding.EncodeToString(v.Data)
		return json.BinData{v.Kind, data}, nil

	case mgo.DBRef: // DBRef
		return json.DBRef{v.Collection, v.Id, v.Database}, nil

	case bson.RegEx: // RegExp
		return json.RegExp{v.Pattern, v.Options}, nil

	case bson.MongoTimestamp: // Timestamp
		inc := uint32(int64(v) & 0xffff)
		secs := uint32((int64(v) & (0xffff << 32)) >> 32)
		return json.Timestamp{secs, inc}, nil

	default:
		switch x {
		case bson.MinKey: // MinKey
			return json.MinKey{}, nil

		case bson.MaxKey: // MaxKey
			return json.MaxKey{}, nil

		case bson.Undefined: // undefined
			return json.Undefined{}, nil
		}
	}

	return nil, fmt.Errorf("Conversion of BSON type '%v' not supported %v", reflect.TypeOf(x), x)
}
