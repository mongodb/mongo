package mongoreplay

import (
	"bytes"
	"fmt"
	"io"
	"io/ioutil"

	mgo "github.com/10gen/llmgo"
)

// RawOp may be exactly the same as OpUnknown.
type RawOp struct {
	Header MsgHeader
	Body   []byte
}

func (op *RawOp) String() string {
	return fmt.Sprintf("RawOp: %v", op.Header.OpCode)
}

// Abbreviated returns a serialization of the RawOp, abbreviated so it
// doesn't exceed the given number of characters.
func (op *RawOp) Abbreviated(chars int) string {
	return fmt.Sprintf("%v", op)
}

// OpCode returns the OpCode for the op.
func (op *RawOp) OpCode() OpCode {
	return op.Header.OpCode
}

// FromReader extracts data from a serialized op into its concrete
// structure.
func (op *RawOp) FromReader(r io.Reader) error {
	if op.Header.MessageLength < MsgHeaderLen {
		return nil
	}
	if op.Header.MessageLength > MaxMessageSize {
		return fmt.Errorf("wire message size, %v, was greater then the maximum, %v bytes", op.Header.MessageLength, MaxMessageSize)
	}
	tempBody := make([]byte, op.Header.MessageLength-MsgHeaderLen)
	_, err := io.ReadFull(r, tempBody)

	if op.Body != nil {
		op.Body = append(op.Body, tempBody...)
	} else {
		op.Body = tempBody
	}
	return err
}

// ShortReplyFromReader reads an op from the given reader. It only holds on
// to header-related information
func (op *RawOp) ShortReplyFromReader(r io.Reader) error {
	if op.Header.MessageLength < MsgHeaderLen {
		return nil
	}
	if op.Header.MessageLength > MaxMessageSize {
		return fmt.Errorf("wire message size, %v, was greater then the maximum, %v bytes", op.Header.MessageLength, MaxMessageSize)
	}
	op.Body = make([]byte, 20) // op_replies have an additional 20 bytes of header that we capture
	_, err := io.ReadFull(r, op.Body)
	if err != nil {
		return err
	}
	_, err = io.CopyN(ioutil.Discard, r, int64(op.Header.MessageLength-MsgHeaderLen-20))
	return err
}

// Parse returns the underlying op from its given RawOp form.
func (op *RawOp) Parse() (Op, error) {
	if op.Header.OpCode == OpCodeCompressed {
		newMsg, err := mgo.DecompressMessage(op.Body)
		if err != nil {
			return nil, err
		}
		op.Header.FromWire(newMsg)
		op.Body = newMsg
	}

	var parsedOp Op
	switch op.Header.OpCode {
	case OpCodeQuery:
		parsedOp = &QueryOp{Header: op.Header}
	case OpCodeReply:
		parsedOp = &ReplyOp{Header: op.Header}
	case OpCodeGetMore:
		parsedOp = &GetMoreOp{Header: op.Header}
	case OpCodeInsert:
		parsedOp = &InsertOp{Header: op.Header}
	case OpCodeKillCursors:
		parsedOp = &KillCursorsOp{Header: op.Header}
	case OpCodeDelete:
		parsedOp = &DeleteOp{Header: op.Header}
	case OpCodeUpdate:
		parsedOp = &UpdateOp{Header: op.Header}
	case OpCodeCommand:
		parsedOp = &CommandOp{Header: op.Header}
	case OpCodeCommandReply:
		parsedOp = &CommandReplyOp{Header: op.Header}
	default:
		return nil, nil
	}
	reader := bytes.NewReader(op.Body[MsgHeaderLen:])
	err := parsedOp.FromReader(reader)
	if err != nil {
		return nil, err
	}
	// Special case to check if this commandOp contains a cursor, which
	// means it needs to be remapped at some point.
	if commandOp, ok := parsedOp.(*CommandOp); ok {
		if commandOp.CommandName == "getMore" {
			return &CommandGetMore{
				CommandOp: *commandOp,
			}, nil
		}
	}
	return parsedOp, nil

}
