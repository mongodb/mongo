package mongoreplay

import (
	"encoding/json"
	"fmt"
	"io"

	mgo "github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

// DeleteOp is used to remove one or more documents from a collection.
// http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#op-delete
type DeleteOp struct {
	Header MsgHeader
	mgo.DeleteOp
}

// Meta returns metadata about the operation, useful for analysis of traffic.
func (op *DeleteOp) Meta() OpMetadata {
	return OpMetadata{
		"Delete",
		op.Collection,
		"",
		op.Selector,
	}
}

func (op *DeleteOp) String() string {
	body, err := op.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("DeleteOp %v %v", op.Collection, body)
}

func (op *DeleteOp) getOpBodyString() (string, error) {
	jsonDoc, err := ConvertBSONValueToJSON(op.Selector)
	if err != nil {
		return "", fmt.Errorf("%#v - %v", op, err)
	}
	selectorAsJSON, _ := json.Marshal(jsonDoc)
	return string(selectorAsJSON), nil
}

// Abbreviated returns a serialization of the DeleteOp, abbreviated so it
// doesn't exceed the given number of characters.
func (op *DeleteOp) Abbreviated(chars int) string {
	body, err := op.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("DeleteOp %v %v", op.Collection, Abbreviate(body, chars))
}

// OpCode returns the OpCode for DeleteOp.
func (op *DeleteOp) OpCode() OpCode {
	return OpCodeDelete
}

// FromReader extracts data from a serialized DeleteOp into its concrete
// structure.
func (op *DeleteOp) FromReader(r io.Reader) error {
	var b [4]byte
	_, err := io.ReadFull(r, b[:]) //skip ZERO
	if err != nil {
		return err
	}
	name, err := readCStringFromReader(r)
	if err != nil {
		return err
	}
	op.Collection = string(name)
	_, err = io.ReadFull(r, b[:]) //Grab the flags
	if err != nil {
		return err
	}
	op.Flags = uint32(getInt32(b[:], 0))

	selectorAsSlice, err := ReadDocument(r)
	if err != nil {
		return err
	}
	op.Selector = &bson.D{}
	err = bson.Unmarshal(selectorAsSlice, op.Selector)

	if err != nil {
		return err
	}
	return nil
}

// Execute performs the DeleteOp on a given session, yielding the reply when
// successful (and an error otherwise).
func (op *DeleteOp) Execute(session *mgo.Session) (Replyable, error) {
	session.SetSocketTimeout(0)
	if err := mgo.ExecOpWithoutReply(session, &op.DeleteOp); err != nil {
		return nil, err
	}
	return nil, nil
}
