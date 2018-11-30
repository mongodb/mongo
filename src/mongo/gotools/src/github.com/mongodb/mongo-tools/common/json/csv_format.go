// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

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

func (js JavaScript) String() string {
	return js.Code
}

func (d Date) String() string {
	if d.isFormatable() {
		n := int64(d)
		t := time.Unix(n/1e3, n%1e3*1e6)
		return t.UTC().Format(JSON_DATE_FORMAT)
	}
	// date.MarshalJSON always returns a nil err.
	data, _ := d.MarshalJSON()
	return string(data)
}

func (d DBRef) String() string {
	return fmt.Sprintf(`{ "$ref": "%v", "$id": %v, "$db": "%v" }`,
		d.Collection, d.Id, d.Database)
}

func (d DBPointer) String() string {
	return fmt.Sprintf(`{ "$ref": "%v", "$id": %v }`,
		d.Namespace, d.Id)
}

func (f Float) String() string {
	return fmt.Sprintf("%v", float64(f))
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
	return fmt.Sprintf("ObjectId(%v)", string(o))
}

func (r RegExp) String() string {
	return fmt.Sprintf("/%v/%v", r.Pattern, r.Options)
}

func (t Timestamp) String() string {
	return fmt.Sprintf(`{ "$timestamp": { "t": %v, "i": %v } }`,
		t.Seconds, t.Increment)
}

func (_ Undefined) String() string {
	return `{ "$undefined": true }`
}
