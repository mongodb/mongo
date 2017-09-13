package mongoreplay

import (
	"bytes"
	"encoding/base64"
	"errors"
	"fmt"
	"io"
	"reflect"
	"strings"
	"time"

	mgo "github.com/10gen/llmgo"
	bson "github.com/10gen/llmgo/bson"
	"github.com/mongodb/mongo-tools/common/json"
)

var (
	// ErrInvalidSize means the size of the BSON document is invalid
	ErrInvalidSize = errors.New("got invalid document size")
)

const (
	maximumDocumentSize = 49 * 1024 * 1024 // there is a 48MB max message size
)

// AbbreviateBytes returns a reduced byte array of the given one if it's
// longer than maxLen by showing only a prefix and suffix of size windowLen
// with an ellipsis in the middle.
func AbbreviateBytes(data []byte, maxLen int) []byte {
	if len(data) <= maxLen {
		return data
	}
	windowLen := (maxLen - 3) / 2
	o := bytes.NewBuffer(data[0:windowLen])
	o.WriteString("...")
	o.Write(data[len(data)-windowLen:])
	return o.Bytes()
}

// Abbreviate returns a reduced copy of the given string if it's longer than
// maxLen by showing only a prefix and suffix of size windowLen with an ellipsis
// in the middle.
func Abbreviate(data string, maxLen int) string {
	if len(data) <= maxLen {
		return data
	}
	windowLen := (maxLen - 3) / 2
	return data[0:windowLen] + "..." + data[len(data)-windowLen:]
}

// CopyMessage copies reads & writes an entire message.
func CopyMessage(w io.Writer, r io.Reader) error {
	h, err := ReadHeader(r)
	if err != nil {
		return err
	}
	if _, err := h.WriteTo(w); err != nil {
		return err
	}
	_, err = io.CopyN(w, r, int64(h.MessageLength-MsgHeaderLen))
	return err
}

// ReadDocument read an entire BSON document. This document can be used with
// bson.Unmarshal.
func ReadDocument(r io.Reader) (doc []byte, err error) {
	sizeRaw := make([]byte, 4)
	if _, err = io.ReadFull(r, sizeRaw); err != nil {
		return
	}

	size := getInt32(sizeRaw, 0)
	if size < 5 || size > maximumDocumentSize {
		err = ErrInvalidSize
		return
	}
	doc = make([]byte, size)
	if size < 4 {
		return
	}
	copy(doc, sizeRaw)

	_, err = io.ReadFull(r, doc[4:])
	return
}

func getCommandName(rawOp *RawOp) (string, error) {
	if rawOp.Header.OpCode != OpCodeCommand {
		return "", fmt.Errorf("getCommandName received wrong opType: %v", rawOp.Header.OpCode)
	}
	reader := bytes.NewReader(rawOp.Body[MsgHeaderLen:])

	_, err := readCStringFromReader(reader)
	if err != nil {
		return "", err
	}

	commandName, err := readCStringFromReader(reader)
	if err != nil {
		return "", err
	}
	return string(commandName), nil
}

func cacheKey(op *RecordedOp, response bool) string {
	var src, dst string
	var id int32
	if !response {
		src = op.SrcEndpoint
		dst = op.DstEndpoint
		id = op.Header.RequestID
	} else {
		src = op.DstEndpoint
		dst = op.SrcEndpoint
		id = op.Header.ResponseTo
	}
	return fmt.Sprintf("%v:%v:%d:%v", src, dst, id, op.Generation)
}

// extractErrors inspects a bson doc and returns all of the mongodb errors contained within.
func extractErrorsFromDoc(doc *bson.D) []error {
	// errors may exist in the following places in the returned document:
	// - the "$err" field, which is set if bit #1 is set on the responseFlags
	// - the "errmsg" field on the top-level returned document
	// - the "writeErrors" and "writeConcernErrors" arrays, which contain objects
	//   that each have an "errmsg" field
	errors := []error{}

	if val, ok := FindValueByKey("$err", doc); ok {
		errors = append(errors, fmt.Errorf("%v", val))
	}

	if val, ok := FindValueByKey("errmsg", doc); ok {
		errors = append(errors, fmt.Errorf("%v", val))
	}

	if val, ok := FindValueByKey("writeErrors", doc); ok {
		if reflect.TypeOf(val).Kind() == reflect.Slice {
			s := reflect.ValueOf(val)
			for i := 0; i < s.Len(); i++ {
				errors = append(errors, fmt.Errorf("%v", s.Index(i).Interface()))
			}
		}
	}

	if val, ok := FindValueByKey("writeConcernErrors", doc); ok {
		if reflect.TypeOf(val).Kind() == reflect.Slice {
			s := reflect.ValueOf(val)
			for i := 0; i < s.Len(); i++ {
				errors = append(errors, fmt.Errorf("%v", s.Index(i).Interface()))
			}
		}
	}

	return errors
}

// readCStringFromReader reads a null turminated string from an io.Reader.
func readCStringFromReader(r io.Reader) ([]byte, error) {
	var b []byte
	var n [1]byte
	for {
		if _, err := io.ReadFull(r, n[:]); err != nil {
			return nil, err
		}
		if n[0] == 0 {
			return b, nil
		}
		b = append(b, n[0])
	}
}

func readCString(b []byte) string {
	for i := 0; i < len(b); i++ {
		if b[i] == 0 {
			return string(b[:i])
		}
	}
	return ""
}

// retrieves a 32 bit into from the given byte array whose first byte is in position pos
// Taken from gopkg.in/mgo.v2/socket.go
func getInt32(b []byte, pos int) int32 {
	return (int32(b[pos+0])) |
		(int32(b[pos+1]) << 8) |
		(int32(b[pos+2]) << 16) |
		(int32(b[pos+3]) << 24)
}

// SetInt32 sets the 32-bit int into the given byte array at position post
// Taken from gopkg.in/mgo.v2/socket.go
func SetInt32(b []byte, pos int, i int32) {
	b[pos] = byte(i)
	b[pos+1] = byte(i >> 8)
	b[pos+2] = byte(i >> 16)
	b[pos+3] = byte(i >> 24)
}

// retrieves a 64 bit into from the given byte array whose first byte is in position pos
// Taken from gopkg.in/mgo.v2/socket.go
func getInt64(b []byte, pos int) int64 {
	return (int64(b[pos+0])) |
		(int64(b[pos+1]) << 8) |
		(int64(b[pos+2]) << 16) |
		(int64(b[pos+3]) << 24) |
		(int64(b[pos+4]) << 32) |
		(int64(b[pos+5]) << 40) |
		(int64(b[pos+6]) << 48) |
		(int64(b[pos+7]) << 56)
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

// SetInt64 sets the 64-bit int into the given byte array at position post
// Taken from gopkg.in/mgo.v2/socket.go
func SetInt64(b []byte, pos int, i int64) {
	b[pos] = byte(i)
	b[pos+1] = byte(i >> 8)
	b[pos+2] = byte(i >> 16)
	b[pos+3] = byte(i >> 24)
	b[pos+4] = byte(i >> 32)
	b[pos+5] = byte(i >> 40)
	b[pos+6] = byte(i >> 48)
	b[pos+7] = byte(i >> 56)
}

// ConvertBSONValueToJSON walks through a document or an array and converts any
// BSON value to its corresponding extended JSON type. It returns the converted
// JSON document and any error encountered.
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
	case []bson.Raw:
		out := make([]interface{}, len(v))
		for i, value := range v {
			out[i] = value
		}
		return ConvertBSONValueToJSON(out)
	case bson.Raw:
		// Unmarshal the raw into a bson.D, then process that.
		convertedFromRaw := bson.D{}
		err := v.Unmarshal(&convertedFromRaw)
		if err != nil {
			return nil, err
		}
		return ConvertBSONValueToJSON(convertedFromRaw)
	case (*bson.Raw):
		return ConvertBSONValueToJSON(*v)
	case (*bson.D):
		return ConvertBSONValueToJSON(*v)
	case bson.D:
		for i, value := range v {
			jsonValue, err := ConvertBSONValueToJSON(value.Value)
			if err != nil {
				return nil, err
			}
			v[i].Value = jsonValue
		}
		return v.Map(), nil
	case []bson.D:
		out := make([]interface{}, len(v))
		for i, value := range v {
			out[i] = value
		}
		return ConvertBSONValueToJSON(out)
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
		return map[string]interface{}{"$ref": v.Collection, "$id": v.Id}, nil

	//case bson.DBPointer: // DBPointer
	//return json.DBPointer{v.Namespace, v.Id}, nil

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

	if valueOfX := reflect.ValueOf(x); valueOfX.Kind() == reflect.Slice || valueOfX.Kind() == reflect.Array {
		result := make([]interface{}, 0, valueOfX.Len())
		for i := 0; i < (valueOfX.Len()); i++ {
			v := valueOfX.Index(i).Interface()
			jsonResult, err := ConvertBSONValueToJSON(v)
			if err != nil {
				return nil, err
			}
			result = append(result, jsonResult)
		}
		return result, nil

	}

	return nil, fmt.Errorf("conversion of BSON type '%v' not supported %v", reflect.TypeOf(x), x)
}

// PreciseTime wraps a time.Time with addition useful methods
type PreciseTime struct {
	time.Time
}

type preciseTimeDecoder struct {
	Sec  int64 `bson:"sec"`
	Nsec int32 `bson:"nsec"`
}

const (
	// Time.Unix() returns the number of seconds from the unix epoch but time's
	// underlying struct stores 'sec' as the number of seconds elapsed since
	// January 1, year 1 00:00:00 UTC
	// This calculation allows for conversion between the internal representation
	// and the UTC representation
	unixToInternal int64 = (1969*365 + 1969/4 - 1969/100 + 1969/400) * 86400

	internalToUnix int64 = -unixToInternal
)

// GetBSON encodes the time into BSON
func (b *PreciseTime) GetBSON() (interface{}, error) {
	result := preciseTimeDecoder{
		Sec:  b.Unix() + unixToInternal,
		Nsec: int32(b.Nanosecond()),
	}
	return &result, nil

}

// SetBSON decodes the BSON into a time
func (b *PreciseTime) SetBSON(raw bson.Raw) error {
	decoder := preciseTimeDecoder{}
	bsonErr := raw.Unmarshal(&decoder)
	if bsonErr != nil {
		return bsonErr
	}

	t := time.Unix(decoder.Sec+internalToUnix, int64(decoder.Nsec))
	b.Time = t.UTC()
	return nil
}

// bufferWaiter is a channel-like structure which only recieves a buffer
// from its channel once on the first Get() call, then yields the same
// buffer upon subsequent Get() calls.
type bufferWaiter struct {
	ch  <-chan *bytes.Buffer
	buf *bytes.Buffer
	rcv bool
}

func newBufferWaiter(ch <-chan *bytes.Buffer) *bufferWaiter {
	return &bufferWaiter{ch: ch}
}

func (b *bufferWaiter) Get() *bytes.Buffer {
	if !b.rcv {
		b.buf = <-b.ch
		b.rcv = true
	}
	return b.buf
}

func getDotField(m map[string]interface{}, key string) (v interface{}, ok bool) {
	s := strings.SplitN(key, ".", 2)
	if len(s) == 0 {
		ok = false
		return
	}
	v, ok = m[s[0]]
	if !ok || len(s) == 1 {
		return
	}
	// more depth required
	nm, ok := v.(map[string]interface{})
	if !ok {
		return
	}
	return getDotField(nm, s[1])
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
