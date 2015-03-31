package archive

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/db"
	"io"
)

// parser.go implements the parsing of the low-level archive format
// The low level archive format is defined as zero or more blocks
// where each block is defined as:
//   a header BSON document
//   zero or more body BSON documents
//   a four byte terminator (0xFFFFFFFF)

type ParserConsumer interface {
	HeaderBSON([]byte) error
	BodyBSON([]byte) error
	End() error
}

type Parser struct {
	In     io.Reader
	buf    [db.MaxBSONSize]byte
	length int
}

type ParserError struct {
	Err error
	Msg string
}

func (pe *ParserError) Error() string {
	err := fmt.Sprintf("corruption found in archive; %v", pe.Msg)
	if pe.Err != nil {
		err = fmt.Sprintf("%v ( %v )", err, pe.Err)
	}
	return err
}

func parserError(msg string) error {
	return &ParserError{
		Msg: msg,
	}
}

func parserErrError(msg string, err error) error {
	return &ParserError{
		Err: err,
		Msg: msg,
	}
}

// readBSONOrTerminator
func (parse *Parser) readBSONOrTerminator() (bool, error) {
	parse.length = 0
	_, err := io.ReadAtLeast(parse.In, parse.buf[0:4], 4)
	if err == io.EOF {
		return false, err
	}
	if err != nil {
		return false, parserErrError("head length or terminator", err)
	}
	size := int32(
		(uint32(parse.buf[0]) << 0) |
			(uint32(parse.buf[1]) << 8) |
			(uint32(parse.buf[2]) << 16) |
			(uint32(parse.buf[3]) << 24),
	)
	if size == terminator {
		return true, nil
	}
	if size < minBSONSize || size > db.MaxBSONSize {
		return false, parserError(fmt.Sprintf("%v is neither a valid bson length nor a archive terminator", size))
	}
	// TODO Because we're reusing this same buffer for all of our IO, we are basically guaranteeing that we'll
	// copy the bytes twice.  At some point we should fix this. It's slightly complex, because we'll need consumer
	// methods closing one buffer and acquiring another
	_, err = io.ReadAtLeast(parse.In, parse.buf[4:size], int(size)-4)
	if err != nil {
		// any error, including EOF is an error so we wrap it up
		return false, parserErrError("read bson", err)
	}
	if parse.buf[size-1] != 0x00 {
		return false, parserError("bson doesn't end with a null byte")
	}
	parse.length = int(size)
	return false, nil
}

func (parse *Parser) ReadAllBlocks(consumer ParserConsumer) (err error) {
	for err == nil {
		err = parse.ReadBlock(consumer)
	}
	if err == io.EOF {
		return nil
	}
	return err
}

func (parse *Parser) ReadBlock(consumer ParserConsumer) (err error) {
	isTerminator, err := parse.readBSONOrTerminator()
	if err == io.EOF {
		handlerErr := consumer.End()
		if handlerErr != nil {
			return parserErrError("ParserConsumer.End", handlerErr)
		}
		return err
	}
	if err != nil {
		return err
	}
	if isTerminator {
		return parserError("consecutive terminators / headerless blocks are not allowed")
	}
	err = consumer.HeaderBSON(parse.buf[:parse.length])
	if err != nil {
		return parserErrError("ParserConsumer.HeaderBSON()", err)
	}
	for {
		isTerminator, err = parse.readBSONOrTerminator()
		if err != nil { // all errors, including EOF are errors here
			return err
		}
		if isTerminator {
			return nil
		}
		err = consumer.BodyBSON(parse.buf[:parse.length])
		if err != nil {
			return parserErrError("ParserConsumer.BodyBSON()", err)
		}
	}
}
