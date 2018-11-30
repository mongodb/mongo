// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoimport

import (
	gocsv "encoding/csv"
	"fmt"
	"io"

	"github.com/mongodb/mongo-tools/mongoimport/csv"
	"gopkg.in/mgo.v2/bson"
)

// CSVInputReader implements the InputReader interface for CSV input types.
type CSVInputReader struct {
	// colSpecs is a list of column specifications in the BSON documents to be imported
	colSpecs []ColumnSpec

	// csvReader is the underlying reader used to read data in from the CSV or CSV file
	csvReader *csv.Reader

	// csvRejectWriter is where coercion-failed rows are written, if applicable
	csvRejectWriter *gocsv.Writer

	// csvRecord stores each line of input we read from the underlying reader
	csvRecord []string

	// numProcessed tracks the number of CSV records processed by the underlying reader
	numProcessed uint64

	// numDecoders is the number of concurrent goroutines to use for decoding
	numDecoders int

	// embedded sizeTracker exposes the Size() method to check the number of bytes read so far
	sizeTracker

	// ignoreBlanks is whether empty fields should be ignored
	ignoreBlanks bool
}

// CSVConverter implements the Converter interface for CSV input.
type CSVConverter struct {
	colSpecs     []ColumnSpec
	data         []string
	index        uint64
	ignoreBlanks bool
	rejectWriter *gocsv.Writer
}

// NewCSVInputReader returns a CSVInputReader configured to read data from the
// given io.Reader, extracting only the specified columns using exactly "numDecoders"
// goroutines.
func NewCSVInputReader(colSpecs []ColumnSpec, in io.Reader, rejects io.Writer, numDecoders int, ignoreBlanks bool) *CSVInputReader {
	szCount := newSizeTrackingReader(newBomDiscardingReader(in))
	csvReader := csv.NewReader(szCount)
	// allow variable number of colSpecs in document
	csvReader.FieldsPerRecord = -1
	csvReader.TrimLeadingSpace = true
	return &CSVInputReader{
		colSpecs:        colSpecs,
		csvReader:       csvReader,
		csvRejectWriter: gocsv.NewWriter(rejects),
		numProcessed:    uint64(0),
		numDecoders:     numDecoders,
		sizeTracker:     szCount,
		ignoreBlanks:    ignoreBlanks,
	}
}

// ReadAndValidateHeader reads the header from the underlying reader and validates
// the header fields. It sets err if the read/validation fails.
func (r *CSVInputReader) ReadAndValidateHeader() (err error) {
	fields, err := r.csvReader.Read()
	if err != nil {
		return err
	}
	r.colSpecs = ParseAutoHeaders(fields)
	return validateReaderFields(ColumnNames(r.colSpecs))
}

// ReadAndValidateHeader reads the header from the underlying reader and validates
// the header fields. It sets err if the read/validation fails.
func (r *CSVInputReader) ReadAndValidateTypedHeader(parseGrace ParseGrace) (err error) {
	fields, err := r.csvReader.Read()
	if err != nil {
		return err
	}
	r.colSpecs, err = ParseTypedHeaders(fields, parseGrace)
	if err != nil {
		return err
	}
	return validateReaderFields(ColumnNames(r.colSpecs))
}

// StreamDocument takes a boolean indicating if the documents should be streamed
// in read order and a channel on which to stream the documents processed from
// the underlying reader. Returns a non-nil error if streaming fails.
func (r *CSVInputReader) StreamDocument(ordered bool, readDocs chan bson.D) (retErr error) {
	csvRecordChan := make(chan Converter, r.numDecoders)
	csvErrChan := make(chan error)

	// begin reading from source
	go func() {
		var err error
		for {
			r.csvRecord, err = r.csvReader.Read()
			if err != nil {
				close(csvRecordChan)
				if err == io.EOF {
					csvErrChan <- nil
				} else {
					r.numProcessed++
					csvErrChan <- fmt.Errorf("read error on entry #%v: %v", r.numProcessed, err)
				}
				return
			}
			csvRecordChan <- CSVConverter{
				colSpecs:     r.colSpecs,
				data:         r.csvRecord,
				index:        r.numProcessed,
				ignoreBlanks: r.ignoreBlanks,
				rejectWriter: r.csvRejectWriter,
			}
			r.numProcessed++
		}
	}()

	go func() {
		csvErrChan <- streamDocuments(ordered, r.numDecoders, csvRecordChan, readDocs)
	}()

	return channelQuorumError(csvErrChan, 2)
}

// Convert implements the Converter interface for CSV input. It converts a
// CSVConverter struct to a BSON document.
func (c CSVConverter) Convert() (b bson.D, err error) {
	b, err = tokensToBSON(
		c.colSpecs,
		c.data,
		c.index,
		c.ignoreBlanks,
	)
	if _, ok := err.(coercionError); ok {
		c.Print()
		err = nil
	}
	return
}

func (c CSVConverter) Print() {
	c.rejectWriter.Write(c.data)
}
