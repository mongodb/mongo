package json

import (
	"encoding/base64"
	"fmt"
	"time"
)

const CSV_DATE_FORMAT = "2006-01-02T15:04:05.000Z"

func (b BinData) String() string {
	data, err := base64.StdEncoding.DecodeString(b.Base64)
	if err != nil {
		return "" // XXX: panic?
	}
	if b.Type == 0x02 {
		data = data[4:] // skip the first 4 bytes
	}
	return fmt.Sprintf("%X", data) // use uppercase hexadecimal
}

func (d Date) String() string {
	nsec := int64(time.Duration(d) * time.Millisecond)
	t := time.Unix(0, nsec)
	return t.Format(CSV_DATE_FORMAT)
}

func (d DBRef) String() string {
	return fmt.Sprintf(`{ "$ref": "%v", "$id": %v, "$db": "%v" }`,
		d.Collection, d.Id, d.Database)
}

func (_ MinKey) String() string {
	return "$MinKey"
}

func (_ MaxKey) String() string {
	return "$MaxKey"
}

func (n NumberInt) String() string {
	return fmt.Sprintf("%v", int32(n))
}

func (n NumberLong) String() string {
	return fmt.Sprintf("%v", int64(n))
}

// Assumes that o represents a valid ObjectId
// (composed of 24 hexadecimal characters).
func (o ObjectId) String() string {
	return fmt.Sprintf("ObjectId('%v')", string(o))
}

func (r RegExp) String() string {
	// TODO: need to escape forward slashes in pattern
	return fmt.Sprintf("/%v/%v", r.Pattern, r.Options)
}

func (t Timestamp) String() string {
	return fmt.Sprintf(`{ "$timestamp": { "t": %v, "i": %v } }`,
		t.Seconds, t.Increment)
}

func (_ Undefined) String() string {
	return `{ "$undefined": true }`
}
