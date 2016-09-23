package mongoreplay

import (
	"bytes"
	"fmt"
	"io"

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
// to header-related information and the first document.
func (op *RawOp) ShortenReply() error {
	if op.Header.MessageLength < MsgHeaderLen {
		return fmt.Errorf("expected message header to have length: %d bytes but was %d bytes", MsgHeaderLen, op.Header.MessageLength)
	}
	if op.Header.MessageLength > MaxMessageSize {
		return fmt.Errorf("wire message size, %v, was greater then the maximum, %v bytes", op.Header.MessageLength, MaxMessageSize)
	}

	switch op.Header.OpCode {
	case OpCodeReply:
		if op.Header.MessageLength <= 20+MsgHeaderLen {
			//there are no reply docs
			return nil
		}
		firstDocSize := getInt32(op.Body, 20+MsgHeaderLen)
		op.Body = op.Body[0:(20 + MsgHeaderLen + firstDocSize)]

	case OpCodeCommandReply:
		commandReplyDocSize := getInt32(op.Body, MsgHeaderLen)
		metadataDocSize := getInt32(op.Body, int(commandReplyDocSize)+MsgHeaderLen)
		if op.Header.MessageLength <= commandReplyDocSize+metadataDocSize+MsgHeaderLen {
			//there are no reply docs
			return nil
		}
		firstOutputDocSize := getInt32(op.Body, int(commandReplyDocSize+metadataDocSize)+MsgHeaderLen)
		shortReplySize := commandReplyDocSize + metadataDocSize + firstOutputDocSize + MsgHeaderLen
		op.Body = op.Body[0:shortReplySize]

	default:
		return fmt.Errorf("unexpected op type : %v", op.Header.OpCode)
	}
	return nil
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
