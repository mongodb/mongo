package bsonutil

import (
	"encoding/base64"
	"errors"
	"fmt"
	"github.com/mongodb/mongo-tools/common/json"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
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
			bsonValue, err := ParseJSONValue(jsonValue)
			if err != nil {
				return nil, err
			}
			v[key] = bsonValue
		}
		return v, nil
	case bson.D:
		for i := range v {
			var err error
			v[i].Value, err = ParseJSONValue(v[i].Value)
			if err != nil {
				return nil, err
			}
		}
		return v, nil

	case []interface{}: // array
		for i, jsonValue := range v {
			bsonValue, err := ParseJSONValue(jsonValue)
			if err != nil {
				return nil, err
			}
			v[i] = bsonValue
		}
		return v, nil

	case string, float64, int32, int64:
		return v, nil // require no conversion

	case json.ObjectId: // ObjectId
		s := string(v)
		if !bson.IsObjectIdHex(s) {
			return nil, errors.New("expected ObjectId to contain 24 hexadecimal characters")
		}
		return bson.ObjectIdHex(s), nil

	case json.Decimal128:
		return v.Decimal128, nil

	case json.Date: // Date
		n := int64(v)
		return time.Unix(n/1e3, n%1e3*1e6), nil

	case json.ISODate: // ISODate
		n := string(v)
		return util.FormatDate(n)

	case json.NumberLong: // NumberLong
		return int64(v), nil

	case json.NumberInt: // NumberInt
		return int32(v), nil

	case json.NumberFloat: // NumberFloat
		return float64(v), nil
	case json.BinData: // BinData
		data, err := base64.StdEncoding.DecodeString(v.Base64)
		if err != nil {
			return nil, err
		}
		return bson.Binary{v.Type, data}, nil

	case json.DBRef: // DBRef
		var err error
		v.Id, err = ParseJSONValue(v.Id)
		if err != nil {
			return nil, err
		}
		return mgo.DBRef{v.Collection, v.Id, v.Database}, nil

	case json.DBPointer: // DBPointer, for backwards compatibility
		return bson.DBPointer{v.Namespace, v.Id}, nil

	case json.RegExp: // RegExp
		return bson.RegEx{v.Pattern, v.Options}, nil

	case json.Timestamp: // Timestamp
		ts := (int64(v.Seconds) << 32) | int64(v.Increment)
		return bson.MongoTimestamp(ts), nil

	case json.JavaScript: // Javascript
		return bson.JavaScript{v.Code, v.Scope}, nil

	case json.MinKey: // MinKey
		return bson.MinKey, nil

	case json.MaxKey: // MaxKey
		return bson.MaxKey, nil

	case json.Undefined: // undefined
		return bson.Undefined, nil

	default:
		return nil, fmt.Errorf("conversion of JSON value '%v' of type '%T' not supported", v, v)
	}
}

func convertKeys(v bson.M) (bson.M, error) {
	for key, value := range v {
		jsonValue, err := ConvertBSONValueToJSON(value)
		if err != nil {
			return nil, err
		}
		v[key] = jsonValue
	}
	return v, nil
}

func getConvertedKeys(v bson.M) (bson.M, error) {
	out := bson.M{}
	for key, value := range v {
		jsonValue, err := GetBSONValueAsJSON(value)
		if err != nil {
			return nil, err
		}
		out[key] = jsonValue
	}
	return out, nil
}

// ConvertBSONValueToJSON walks through a document or an array and
// converts any BSON value to its corresponding extended JSON type.
// It returns the converted JSON document and any error encountered.
func ConvertBSONValueToJSON(x interface{}) (interface{}, error) {
	switch v := x.(type) {
	case nil:
		return nil, nil
	case bool:
		return v, nil

	case *bson.M: // document
		doc, err := convertKeys(*v)
		if err != nil {
			return nil, err
		}
		return doc, err
	case bson.M: // document
		return convertKeys(v)
	case map[string]interface{}:
		return convertKeys(v)
	case bson.D:
		for i, value := range v {
			jsonValue, err := ConvertBSONValueToJSON(value.Value)
			if err != nil {
				return nil, err
			}
			v[i].Value = jsonValue
		}
		return MarshalD(v), nil
	case MarshalD:
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

	case string:
		return v, nil // require no conversion

	case int:
		return json.NumberInt(v), nil

	case bson.ObjectId: // ObjectId
		return json.ObjectId(v.Hex()), nil

	case bson.Decimal128:
		return json.Decimal128{v}, nil

	case time.Time: // Date
		return json.Date(v.Unix()*1000 + int64(v.Nanosecond()/1e6)), nil

	case int64: // NumberLong
		return json.NumberLong(v), nil

	case int32: // NumberInt
		return json.NumberInt(v), nil

	case float64:
		return json.NumberFloat(v), nil

	case float32:
		return json.NumberFloat(float64(v)), nil

	case []byte: // BinData (with generic type)
		data := base64.StdEncoding.EncodeToString(v)
		return json.BinData{0x00, data}, nil

	case bson.Binary: // BinData
		data := base64.StdEncoding.EncodeToString(v.Data)
		return json.BinData{v.Kind, data}, nil

	case mgo.DBRef: // DBRef
		return json.DBRef{v.Collection, v.Id, v.Database}, nil

	case bson.DBPointer: // DBPointer
		return json.DBPointer{v.Namespace, v.Id}, nil

	case bson.RegEx: // RegExp
		return json.RegExp{v.Pattern, v.Options}, nil

	case bson.MongoTimestamp: // Timestamp
		timestamp := int64(v)
		return json.Timestamp{
			Seconds:   uint32(timestamp >> 32),
			Increment: uint32(timestamp),
		}, nil

	case bson.JavaScript: // JavaScript
		var scope interface{}
		var err error
		if v.Scope != nil {
			scope, err = ConvertBSONValueToJSON(v.Scope)
			if err != nil {
				return nil, err
			}
		}
		return json.JavaScript{v.Code, scope}, nil

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

	return nil, fmt.Errorf("conversion of BSON value '%v' of type '%T' not supported", x, x)
}

// GetBSONValueAsJSON is equivalent to ConvertBSONValueToJSON, but does not mutate its argument.
func GetBSONValueAsJSON(x interface{}) (interface{}, error) {
	switch v := x.(type) {
	case nil:
		return nil, nil
	case bool:
		return v, nil

	case *bson.M: // document
		doc, err := getConvertedKeys(*v)
		if err != nil {
			return nil, err
		}
		return doc, err
	case bson.M: // document
		return getConvertedKeys(v)
	case map[string]interface{}:
		return getConvertedKeys(v)
	case bson.D:
		out := bson.D{}
		for _, value := range v {
			jsonValue, err := GetBSONValueAsJSON(value.Value)
			if err != nil {
				return nil, err
			}
			out = append(out, bson.DocElem{
				Name:  value.Name,
				Value: jsonValue,
			})
		}
		return MarshalD(out), nil
	case MarshalD:
		out, err := GetBSONValueAsJSON(bson.D(v))
		if err != nil {
			return nil, err
		}
		return MarshalD(out.(bson.D)), nil
	case []interface{}: // array
		out := []interface{}{}
		for _, value := range v {
			jsonValue, err := GetBSONValueAsJSON(value)
			if err != nil {
				return nil, err
			}
			out = append(out, jsonValue)
		}
		return out, nil

	case string:
		return v, nil // require no conversion

	case int:
		return json.NumberInt(v), nil

	case bson.ObjectId: // ObjectId
		return json.ObjectId(v.Hex()), nil

	case bson.Decimal128:
		return json.Decimal128{v}, nil

	case time.Time: // Date
		return json.Date(v.Unix()*1000 + int64(v.Nanosecond()/1e6)), nil

	case int64: // NumberLong
		return json.NumberLong(v), nil

	case int32: // NumberInt
		return json.NumberInt(v), nil

	case float64:
		return json.NumberFloat(v), nil

	case float32:
		return json.NumberFloat(float64(v)), nil

	case []byte: // BinData (with generic type)
		data := base64.StdEncoding.EncodeToString(v)
		return json.BinData{0x00, data}, nil

	case bson.Binary: // BinData
		data := base64.StdEncoding.EncodeToString(v.Data)
		return json.BinData{v.Kind, data}, nil

	case mgo.DBRef: // DBRef
		return json.DBRef{v.Collection, v.Id, v.Database}, nil

	case bson.DBPointer: // DBPointer
		return json.DBPointer{v.Namespace, v.Id}, nil

	case bson.RegEx: // RegExp
		return json.RegExp{v.Pattern, v.Options}, nil

	case bson.MongoTimestamp: // Timestamp
		timestamp := int64(v)
		return json.Timestamp{
			Seconds:   uint32(timestamp >> 32),
			Increment: uint32(timestamp),
		}, nil

	case bson.JavaScript: // JavaScript
		var scope interface{}
		var err error
		if v.Scope != nil {
			scope, err = GetBSONValueAsJSON(v.Scope)
			if err != nil {
				return nil, err
			}
		}
		return json.JavaScript{v.Code, scope}, nil

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

	return nil, fmt.Errorf("conversion of BSON value '%v' of type '%T' not supported", x, x)
}
