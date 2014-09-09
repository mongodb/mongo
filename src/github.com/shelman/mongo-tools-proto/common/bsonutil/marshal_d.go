package bsonutil

import (
	"bytes"
	"encoding/json"
	"fmt"
	"gopkg.in/mgo.v2/bson"
)

// MarshalD is a wrapper for bson.D that allows unmarshalling
// of bson.D with preserved order. Necessary for printing
// certain database commands
type MarshalD bson.D

// MarshalJSON makes the MarshalD type usable by
// the encoding/json package
func (md MarshalD) MarshalJSON() ([]byte, error) {
	var buff bytes.Buffer
	buff.WriteString("{")
	for i, item := range md {
		key := fmt.Sprintf(`"%s":`, item.Name)
		val, err := json.Marshal(item.Value)
		if err != nil {
			return nil, fmt.Errorf("cannot marshal %v: %v", item.Value, err)
		}
		buff.WriteString(key)
		buff.Write(val)
		if i != len(md)-1 {
			buff.WriteString(",")
		}
	}
	buff.WriteString("}")
	return buff.Bytes(), nil
}
