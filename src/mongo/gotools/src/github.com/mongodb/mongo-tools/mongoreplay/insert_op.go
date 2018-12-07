package mongoreplay

import (
	"encoding/json"
	"fmt"
	"io"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

// InsertOp is used to insert one or more documents into a collection.
// http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#op-insert
type InsertOp struct {
	Header MsgHeader
	mgo.InsertOp
}

// Meta returns metadata about the InsertOp, useful for analysis of traffic.
func (op *InsertOp) Meta() OpMetadata {
	return OpMetadata{"insert", op.Collection, "", op.Documents}
}

// OpCode returns the OpCode for the InsertOp.
func (op *InsertOp) OpCode() OpCode {
	return OpCodeInsert
}

func (op *InsertOp) String() string {
	body, err := op.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("InsertOp %v %v", op.Collection, body)
}

func (op *InsertOp) getOpBodyString() (string, error) {
	docs := make([]string, 0, len(op.Documents))
	for _, d := range op.Documents {
		jsonDoc, err := ConvertBSONValueToJSON(d)
		if err != nil {
			return "", fmt.Errorf("%#v - %v", op, err)
		}
		asJSON, _ := json.Marshal(jsonDoc)
		docs = append(docs, string(asJSON))
	}
	return fmt.Sprintf("%v", docs), nil
}

// Abbreviated returns a serialization of the InsertOp, abbreviated so it
// doesn't exceed the given number of characters.
func (op *InsertOp) Abbreviated(chars int) string {
	body, err := op.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("InsertOp %v %v", op.Collection, Abbreviate(body, chars))
}

// FromReader extracts data from a serialized InsertOp into its concrete
// structure.
func (op *InsertOp) FromReader(r io.Reader) error {
	var b [4]byte
	_, err := io.ReadFull(r, b[:])
	if err != nil {
		return err
	}
	op.Flags = uint32(getInt32(b[:], 0))
	name, err := readCStringFromReader(r)
	if err != nil {
		return err
	}
	op.Collection = string(name)
	op.Documents = make([]interface{}, 0)

	docLen := 0
	for len(name)+1+4+docLen < int(op.Header.MessageLength)-MsgHeaderLen {
		docAsSlice, err := ReadDocument(r)
		doc := &bson.D{}
		err = bson.Unmarshal(docAsSlice, doc)
		if err != nil {
			return err
		}
		docLen += len(docAsSlice)
		op.Documents = append(op.Documents, doc)
	}
	return nil
}

// Execute performs the InsertOp on a given session, yielding the reply when
// successful (and an error otherwise).
func (op *InsertOp) Execute(session *mgo.Session) (Replyable, error) {
	session.SetSocketTimeout(0)
	if err := mgo.ExecOpWithoutReply(session, &op.InsertOp); err != nil {
		return nil, err
	}

	return nil, nil
}
