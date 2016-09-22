package mongoreplay

import (
	"encoding/json"
	"fmt"
	"io"

	"github.com/10gen/llmgo"
	"github.com/10gen/llmgo/bson"
)

// UpdateOp is used to update a document in a collection.
// http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#op-update
type UpdateOp struct {
	Header MsgHeader
	mgo.UpdateOp
}

// Meta returns metadata about the UpdateOp, useful for analysis of traffic.
func (op *UpdateOp) Meta() OpMetadata {
	return OpMetadata{"update",
		op.Collection,
		"",
		map[string]interface{}{
			"query":  op.Selector,
			"update": op.Update,
		},
	}
}

func (op *UpdateOp) String() string {
	selectorString, updateString, err := op.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("OpUpdate %v %v %v", op.Collection, selectorString, updateString)
}

func (op *UpdateOp) getOpBodyString() (string, string, error) {
	selectorDoc, err := ConvertBSONValueToJSON(op.Selector)
	if err != nil {
		return "", "", fmt.Errorf("ConvertBSONValueToJSON err: %#v - %v", op, err)
	}

	updateDoc, err := ConvertBSONValueToJSON(op.Update)
	if err != nil {
		return "", "", fmt.Errorf("ConvertBSONValueToJSON err: %#v - %v", op, err)
	}
	selectorAsJSON, err := json.Marshal(selectorDoc)
	if err != nil {
		return "", "", fmt.Errorf("json marshal err: %#v - %v", op, err)
	}
	updateAsJSON, err := json.Marshal(updateDoc)
	if err != nil {
		return "", "", fmt.Errorf("json marshal err: %#v - %v", op, err)
	}
	return string(selectorAsJSON), string(updateAsJSON), nil
}

// Abbreviated returns a serialization of the UpdateOp, abbreviated so it
// doesn't exceed the given number of characters.
func (op *UpdateOp) Abbreviated(chars int) string {
	selectorString, updateString, err := op.getOpBodyString()
	if err != nil {
		return fmt.Sprintf("%v", err)
	}
	return fmt.Sprintf("OpQuery %v %v %v", op.Collection, Abbreviate(selectorString, chars), Abbreviate(updateString, chars))
}

// OpCode returns the OpCode for an UpdateOp.
func (op *UpdateOp) OpCode() OpCode {
	return OpCodeUpdate
}

// FromReader extracts data from a serialized UpdateOp into its concrete
// structure.
func (op *UpdateOp) FromReader(r io.Reader) error {
	var b [4]byte
	if _, err := io.ReadFull(r, b[:]); err != nil { // skip ZERO
		return err
	}
	name, err := readCStringFromReader(r)
	if err != nil {
		return err
	}
	op.Collection = string(name)

	if _, err := io.ReadFull(r, b[:]); err != nil { // grab the flags
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

	updateAsSlice, err := ReadDocument(r)
	if err != nil {
		return err
	}
	op.Update = &bson.D{}
	err = bson.Unmarshal(updateAsSlice, op.Update)
	if err != nil {
		return err
	}

	return nil
}

// Execute performs the UpdateOp on a given session, yielding the reply when
// successful (and an error otherwise).
func (op *UpdateOp) Execute(session *mgo.Session) (Replyable, error) {
	if err := mgo.ExecOpWithoutReply(session, &op.UpdateOp); err != nil {
		return nil, err
	}
	return nil, nil
}
