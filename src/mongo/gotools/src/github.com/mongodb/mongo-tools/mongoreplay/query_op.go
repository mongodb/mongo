package mongoreplay

import (
	"fmt"
	"io"
	"strings"
	"time"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
	"github.com/mongodb/mongo-tools/common/json"
)

// QueryOp is used to query the database for documents in a collection.
// http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#op-query
type QueryOp struct {
	Header MsgHeader
	mgo.QueryOp
}

// Meta returns metadata about the QueryOp, useful for analysis of traffic.
func (op *QueryOp) Meta() OpMetadata {
	opType, commandType := extractOpType(op.Query)
	if !strings.HasSuffix(op.Collection, "$cmd") {
		return OpMetadata{"query", op.Collection, "", op.Query}
	}

	return OpMetadata{opType, op.Collection, commandType, op.QueryOp.Query}
}

// extractOpType checks a write command's "query" and determines if it's actually
// an insert, update, delete, or command.
func extractOpType(x interface{}) (string, string) {
	var asMap bson.M
	var commandName string
	switch v := x.(type) {
	case bson.D:
		if len(v) > 0 {
			commandName = v[0].Name
		}
		asMap = v.Map()
	case *bson.M: // document
		asMap = *v
	case *bson.Raw: // document
		return extractOpType(*v)
	case bson.Raw: // document
		asD := bson.D{}
		err := v.Unmarshal(&asD)
		if err != nil {
			panic(fmt.Sprintf("couldn't unmarshal Raw bson into D: %v", err))
		}
		return extractOpType(asD)
	case bson.M: // document
		asMap = v
	case map[string]interface{}:
		asMap = bson.M(v)
	case (*bson.D):
		if de := []bson.DocElem(*v); len(de) > 0 {
			commandName = de[0].Name
		}
		asMap = v.Map()
	}

	for _, v := range []string{"insert", "update", "delete"} {
		if _, ok := asMap[v]; ok {
			return v, ""
		}
	}
	return "command", commandName
}

func (op *QueryOp) String() string {
	body, err := op.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("OpQuery collection:%v %v", op.Collection, body)
}

func (op *QueryOp) getOpBodyString() (string, error) {
	queryAsJSON, err := ConvertBSONValueToJSON(op.Query)
	if err != nil {
		return "", fmt.Errorf("ConvertBSONValueToJSON err: %#v - %v", op, err)
	}
	asJSON, err := json.Marshal(queryAsJSON)
	if err != nil {
		return "", fmt.Errorf("json marshal err: %#v - %v", op, err)
	}
	return string(asJSON), nil
}

// Abbreviated returns a serialization of the QueryOp, abbreviated so it
// doesn't exceed the given number of characters.
func (op *QueryOp) Abbreviated(chars int) string {
	body, err := op.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("OpQuery %v %v", op.Collection, Abbreviate(body, chars))
}

// OpCode returns the OpCode for a QueryOp.
func (op *QueryOp) OpCode() OpCode {
	return OpCodeQuery
}

// FromReader extracts data from a serialized QueryOp into its concrete
// structure.
func (op *QueryOp) FromReader(r io.Reader) error {
	var b [8]byte
	if _, err := io.ReadFull(r, b[:4]); err != nil {
		return err
	}
	op.Flags = mgo.QueryOpFlags(getInt32(b[:], 0))
	name, err := readCStringFromReader(r)
	if err != nil {
		return err
	}
	op.Collection = string(name)

	if _, err := io.ReadFull(r, b[:]); err != nil {
		return err
	}

	op.Skip = getInt32(b[:], 0)
	op.Limit = getInt32(b[:], 4)

	queryAsSlice, err := ReadDocument(r)
	if err != nil {
		return err
	}

	op.Query = &bson.Raw{}
	err = bson.Unmarshal(queryAsSlice, op.Query)
	if err != nil {
		return err
	}
	currentRead := len(queryAsSlice) + len(op.Collection) + 1 + 12 + MsgHeaderLen
	if int(op.Header.MessageLength) > currentRead {
		selectorAsSlice, err := ReadDocument(r)
		if err != nil {
			return err
		}
		op.Selector = &bson.D{}
		err = bson.Unmarshal(selectorAsSlice, op.Selector)
		if err != nil {
			return err
		}
	}
	return nil
}

// Execute performs the QueryOp on a given session, yielding the reply when
// successful (and an error otherwise).
func (op *QueryOp) Execute(session *mgo.Session) (Replyable, error) {
	session.SetSocketTimeout(0)
	before := time.Now()
	_, _, replyData, resultReply, err := mgo.ExecOpWithReply(session, &op.QueryOp)
	after := time.Now()
	if err != nil {
		return nil, err
	}
	mgoReply, ok := resultReply.(*mgo.ReplyOp)
	if !ok {
		panic("reply from execution was not the correct type")
	}
	reply := &ReplyOp{
		ReplyOp: *mgoReply,
	}

	for _, d := range replyData {
		dataDoc := bson.Raw{}
		err = bson.Unmarshal(d, &dataDoc)
		if err != nil {
			return nil, err
		}
		reply.Docs = append(reply.Docs, dataDoc)
	}

	reply.Latency = after.Sub(before)
	return reply, nil
}
