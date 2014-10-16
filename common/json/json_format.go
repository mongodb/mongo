package json

import (
	"fmt"
	"time"
)

const JSON_DATE_FORMAT = "2006-01-02T15:04:05.000Z"

func (b BinData) MarshalJSON() ([]byte, error) {
	data := fmt.Sprintf(`{ "$binary": "%v", "$type": "%0x" }`,
		b.Base64, []byte{b.Type})
	return []byte(data), nil
}

func (js Javascript) MarshalJSON() ([]byte, error) {
	return []byte(js.Code), nil
}

func (d Date) MarshalJSON() ([]byte, error) {
	var data string
	n := int64(d)
	if d.isFormatable() {
		t := time.Unix(n/1e3, n%1e3*1e6)
		data = fmt.Sprintf(`{ "$date": "%v" }`, t.UTC().Format(JSON_DATE_FORMAT))
	} else {
		data = fmt.Sprintf(`{ "$date": { "$numberLong" : "%v" }}`, n)
	}

	return []byte(data), nil
}

func (d DBRef) MarshalJSON() ([]byte, error) {
	// Convert the $id field to JSON
	idChunk, err := Marshal(d.Id)
	if err != nil {
		return nil, err
	}

	// Need to form JSON like { "$ref": "REF", "$id": ID, "$db": "DB" }
	// so piece chunks together since can only get $id field as bytes.
	refChunk := []byte(fmt.Sprintf(`{ "$ref": "%v", "$id": `, d.Collection))

	var dbChunk []byte
	if d.Database != "" {
		dbChunk = []byte(fmt.Sprintf(`, "$db": "%v" }`, d.Database))
	}

	data := make([]byte, len(refChunk)+len(idChunk)+len(dbChunk))
	copy(data, refChunk)
	copy(data[len(refChunk):], idChunk)
	copy(data[len(refChunk)+len(idChunk):], dbChunk)

	return data, nil
}

func (_ MinKey) MarshalJSON() ([]byte, error) {
	data := `{ "$minKey": 1 }`
	return []byte(data), nil
}

func (_ MaxKey) MarshalJSON() ([]byte, error) {
	data := `{ "$maxKey": 1 }`
	return []byte(data), nil
}

func (n NumberInt) MarshalJSON() ([]byte, error) {
	return []byte(fmt.Sprintf("%d", int32(n))), nil
}

func (n NumberLong) MarshalJSON() ([]byte, error) {
	data := fmt.Sprintf(`{ "$numberLong": "%v" }`, int64(n))
	return []byte(data), nil
}

// Assumes that o represents a valid ObjectId
// (composed of 24 hexadecimal characters).
func (o ObjectId) MarshalJSON() ([]byte, error) {
	data := fmt.Sprintf(`{ "$oid": "%v" }`, string(o))
	return []byte(data), nil
}

func (r RegExp) MarshalJSON() ([]byte, error) {
	data := fmt.Sprintf(`{ "$regex": "%v", "$options": "%v" }`,
		r.Pattern, r.Options)
	return []byte(data), nil
}

func (t Timestamp) MarshalJSON() ([]byte, error) {
	data := fmt.Sprintf(`{ "$timestamp": { "t": %v, "i": %v } }`,
		t.Seconds, t.Increment)
	return []byte(data), nil
}

func (_ Undefined) MarshalJSON() ([]byte, error) {
	data := `{ "$undefined": true }`
	return []byte(data), nil
}
