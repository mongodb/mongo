// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoimport

import (
	"bufio"
	"fmt"
	"io"
	"strings"

	"gopkg.in/mgo.v2/bson"
)

const (
	entryDelimiter = '\n'
	tokenSeparator = "\t"
)

// TSVInputReader is a struct that implements the InputReader interface for a
// TSV input source.
type TSVInputReader struct {
	// colSpecs is a list of column specifications in the BSON documents to be imported
	colSpecs []ColumnSpec

	// tsvReader is the underlying reader used to read data in from the TSV
	// or TSV file
	tsvReader *bufio.Reader

	// tsvRejectWriter is where coercion-failed rows are written, if applicable
	tsvRejectWriter io.Writer

	// tsvRecord stores each line of input we read from the underlying reader
	tsvRecord string

	// numProcessed tracks the number of TSV records processed by the underlying reader
	numProcessed uint64

	// numDecoders is the number of concurrent goroutines to use for decoding
	numDecoders int

	// embedded sizeTracker exposes the Size() method to check the number of bytes read so far
	sizeTracker

	// ignoreBlanks is whether empty fields should be ignored
	ignoreBlanks bool
}

// TSVConverter implements the Converter interface for TSV input.
type TSVConverter struct {
	colSpecs     []ColumnSpec
	data         string
	index        uint64
	ignoreBlanks bool
	rejectWriter io.Writer
}

// NewTSVInputReader returns a TSVInputReader configured to read input from the
// given io.Reader, extracting the specified columns only.
func NewTSVInputReader(colSpecs []ColumnSpec, in io.Reader, rejects io.Writer, numDecoders int, ignoreBlanks bool) *TSVInputReader {
	szCount := newSizeTrackingReader(newBomDiscardingReader(in))
	return &TSVInputReader{
		colSpecs:        colSpecs,
		tsvReader:       bufio.NewReader(szCount),
		tsvRejectWriter: rejects,
		numProcessed:    uint64(0),
		numDecoders:     numDecoders,
		sizeTracker:     szCount,
		ignoreBlanks:    ignoreBlanks,
	}
}

// ReadAndValidateHeader reads the header from the underlying reader and validates
// the header fields. It sets err if the read/validation fails.
func (r *TSVInputReader) ReadAndValidateHeader() (err error) {
	header, err := r.tsvReader.ReadString(entryDelimiter)
	if err != nil {
		return err
	}
	for _, field := range strings.Split(header, tokenSeparator) {
		r.colSpecs = append(r.colSpecs, ColumnSpec{
			Name:   strings.TrimRight(field, "\r\n"),
			Parser: new(FieldAutoParser),
		})
	}
	return validateReaderFields(ColumnNames(r.colSpecs))
}

// ReadAndValidateTypedHeader reads the header from the underlying reader and validates
// the header fields. It sets err if the read/validation fails.
func (r *TSVInputReader) ReadAndValidateTypedHeader(parseGrace ParseGrace) (err error) {
	header, err := r.tsvReader.ReadString(entryDelimiter)
	if err != nil {
		return err
	}
	var headerFields []string
	for _, field := range strings.Split(header, tokenSeparator) {
		headerFields = append(headerFields, strings.TrimRight(field, "\r\n"))
	}
	r.colSpecs, err = ParseTypedHeaders(headerFields, parseGrace)
	if err != nil {
		return err
	}
	return validateReaderFields(ColumnNames(r.colSpecs))
}

// StreamDocument takes a boolean indicating if the documents should be streamed
// in read order and a channel on which to stream the documents processed from
// the underlying reader. Returns a non-nil error if streaming fails.
func (r *TSVInputReader) StreamDocument(ordered bool, readDocs chan bson.D) (retErr error) {
	tsvRecordChan := make(chan Converter, r.numDecoders)
	tsvErrChan := make(chan error)

	// begin reading from source
	go func() {
		var err error
		for {
			r.tsvRecord, err = r.tsvReader.ReadString(entryDelimiter)
			if err != nil {
				close(tsvRecordChan)
				if err == io.EOF {
					tsvErrChan <- nil
				} else {
					r.numProcessed++
					tsvErrChan <- fmt.Errorf("read error on entry #%v: %v", r.numProcessed, err)
				}
				return
			}
			tsvRecordChan <- TSVConverter{
				colSpecs:     r.colSpecs,
				data:         r.tsvRecord,
				index:        r.numProcessed,
				ignoreBlanks: r.ignoreBlanks,
				rejectWriter: r.tsvRejectWriter,
			}
			r.numProcessed++
		}
	}()

	// begin processing read bytes
	go func() {
		tsvErrChan <- streamDocuments(ordered, r.numDecoders, tsvRecordChan, readDocs)
	}()

	return channelQuorumError(tsvErrChan, 2)
}

// Convert implements the Converter interface for TSV input. It converts a
// TSVConverter struct to a BSON document.
func (c TSVConverter) Convert() (b bson.D, err error) {
	b, err = tokensToBSON(
		c.colSpecs,
		strings.Split(strings.TrimRight(c.data, "\r\n"), tokenSeparator),
		c.index,
		c.ignoreBlanks,
	)
	if _, ok := err.(coercionError); ok {
		c.Print()
		err = nil
	}
	return
}

func (c TSVConverter) Print() {
	c.rejectWriter.Write([]byte(c.data + "\n"))
}
