// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package archive

import (
	"fmt"
	"io"

	"github.com/mongodb/mongo-tools/common/db"
)

// parser.go implements the parsing of the low-level archive format
// The low level archive format is defined as zero or more blocks
// where each block is defined as:
//   a header BSON document
//   zero or more body BSON documents
//   a four byte terminator (0xFFFFFFFF)

// ParserConsumer is the interface that one needs to implement to consume data from the Parser
type ParserConsumer interface {
	HeaderBSON([]byte) error
	BodyBSON([]byte) error
	End() error
}

// Parser encapsulates the small amount of state that the parser needs to keep
type Parser struct {
	In     io.Reader
	buf    [db.MaxBSONSize]byte
	length int
}

type parserError struct {
	Err error
	Msg string
}

// Error is part of the Error interface. It formats a parserError for human readability.
func (pe *parserError) Error() string {
	err := fmt.Sprintf("corruption found in archive; %v", pe.Msg)
	if pe.Err != nil {
		err = fmt.Sprintf("%v ( %v )", err, pe.Err)
	}
	return err
}

// newParserError creates a parserError with just a message
func newParserError(msg string) error {
	return &parserError{
		Msg: msg,
	}
}

// newParserWrappedError creates a parserError with a message as well as an underlying cause error
func newParserWrappedError(msg string, err error) error {
	return &parserError{
		Err: err,
		Msg: msg,
	}
}

// readBSONOrTerminator reads at least four bytes, determines
// if the first four bytes are a terminator, a bson lenght, or something else.
// If they are a terminator, true,nil are returned. If they are a BSON length,
// then the remainder of the BSON document are read in to the parser, otherwise
// an error is returned.
func (parse *Parser) readBSONOrTerminator() (isTerminator bool, err error) {
	parse.length = 0
	_, err = io.ReadFull(parse.In, parse.buf[0:4])
	if err == io.EOF {
		return false, err
	}
	if err != nil {
		return false, newParserWrappedError("I/O error reading length or terminator", err)
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
		return false, newParserError(fmt.Sprintf("%v is neither a valid bson length nor a archive terminator", size))
	}
	// TODO Because we're reusing this same buffer for all of our IO, we are basically guaranteeing that we'll
	// copy the bytes twice.  At some point we should fix this. It's slightly complex, because we'll need consumer
	// methods closing one buffer and acquiring another
	_, err = io.ReadFull(parse.In, parse.buf[4:size])
	if err != nil {
		// any error, including EOF is an error so we wrap it up
		return false, newParserWrappedError("read bson", err)
	}
	if parse.buf[size-1] != 0x00 {
		return false, newParserError(fmt.Sprintf("bson (size: %v, byte: %d) doesn't end with a null byte", size, parse.buf[size-1]))
	}
	parse.length = int(size)
	return false, nil
}

// ReadAllBlocks calls ReadBlock() until it returns an error.
// If the error is EOF, then nil is returned, otherwise it returns the error
func (parse *Parser) ReadAllBlocks(consumer ParserConsumer) (err error) {
	for err == nil {
		err = parse.ReadBlock(consumer)
	}
	endError := consumer.End()
	if err == io.EOF {
		return endError
	}
	return err
}

// ReadBlock reads one archive block ( header + body* + terminator )
// calling consumer.HeaderBSON() on the header, consumer.BodyBSON() on each piece of body,
// and consumer.EOF() when EOF is encountered before any data was read.
// It returns nil if a whole block was read, io.EOF if nothing was read,
// and a parserError if there was any io error in the middle of the block,
// if either of the consumer methods return error, or if there was any sort of
// parsing failure.
func (parse *Parser) ReadBlock(consumer ParserConsumer) (err error) {
	isTerminator, err := parse.readBSONOrTerminator()
	if err != nil {
		return err
	}
	if isTerminator {
		return newParserError("consecutive terminators / headerless blocks are not allowed")
	}
	err = consumer.HeaderBSON(parse.buf[:parse.length])
	if err != nil {
		return newParserWrappedError("ParserConsumer.HeaderBSON()", err)
	}
	for {
		isTerminator, err = parse.readBSONOrTerminator()
		if err != nil { // all errors, including EOF are errors here
			return newParserWrappedError("ParserConsumer.BodyBSON()", err)
		}
		if isTerminator {
			return nil
		}
		err = consumer.BodyBSON(parse.buf[:parse.length])
		if err != nil {
			return newParserWrappedError("ParserConsumer.BodyBSON()", err)
		}
	}
}
