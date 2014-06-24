package mongoexport

import (
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"labix.org/v2/mgo/bson"
	"time"
)

type BSONExt map[string]interface{}

type ObjectIdExt bson.ObjectId
type NumberLongExt int64
type MongoTimestampExt bson.MongoTimestamp
type GenericBinaryExt []byte
type BinaryExt bson.Binary
type JavascriptExt bson.JavaScript
type TimeExt time.Time
type RegExExt bson.RegEx

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
	return []byte("\"" + m.Code + "\""), nil
}
