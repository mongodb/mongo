// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package bsondump converts BSON files into human-readable formats such as JSON.
package bsondump

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"strings"
	"time"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/bson/bsontype"

	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools-common/failpoint"
	"github.com/mongodb/mongo-tools-common/json"
	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/options"
	"github.com/mongodb/mongo-tools-common/util"
)

// BSONDump is a container for the user-specified options and
// internal state used for running bsondump.
type BSONDump struct {
	// generic mongo tool options
	ToolOptions *options.ToolOptions

	// OutputOptions defines options used to control how BSON data is displayed
	OutputOptions *OutputOptions

	// File handle for the output data.
	OutputWriter io.WriteCloser

	InputSource *db.BSONSource
}

type ReadNopCloser struct {
	io.Reader
}

func (ReadNopCloser) Close() error { return nil }

type WriteNopCloser struct {
	io.Writer
}

func (WriteNopCloser) Close() error { return nil }

// GetWriter opens and returns an io.WriteCloser for the OutFileName in OutputOptions
// or nil if none is set. The caller is responsible for closing it.
func (oo *OutputOptions) GetWriter() (io.WriteCloser, error) {
	if oo.OutFileName != "" {
		file, err := os.Create(util.ToUniversalPath(oo.OutFileName))
		if err != nil {
			return nil, err
		}
		return file, nil
	}

	return WriteNopCloser{os.Stdout}, nil
}

// GetBSONReader opens and returns an io.ReadCloser for the BSONFileName in OutputOptions
// or nil if none is set. The caller is responsible for closing it.
func (oo *OutputOptions) GetBSONReader() (io.ReadCloser, error) {
	if oo.BSONFileName != "" {
		file, err := os.Open(util.ToUniversalPath(oo.BSONFileName))
		if err != nil {
			return nil, fmt.Errorf("couldn't open BSON file: %v", err)
		}
		return file, nil
	}
	return ReadNopCloser{os.Stdin}, nil
}

// New constructs a new instance of BSONDump configured by the provided options.
// A successfully created instance must be closed with Close().
func New(opts Options) (*BSONDump, error) {
	dumper := &BSONDump{
		ToolOptions:   opts.ToolOptions,
		OutputOptions: opts.OutputOptions,
	}

	reader, err := opts.GetBSONReader()
	if err != nil {
		return nil, fmt.Errorf("getting BSON reader failed: %v", err)
	}
	dumper.InputSource = db.NewBSONSource(reader)

	writer, err := opts.GetWriter()
	if err != nil {
		_ = dumper.InputSource.Close()
		return nil, fmt.Errorf("getting Writer failed: %v", err)
	}
	dumper.OutputWriter = writer

	return dumper, nil
}

// Close cleans up the internal state of the given BSONDump instance. The instance should not be used again
// after Close is called.
func (bd *BSONDump) Close() error {
	_ = bd.InputSource.Close()
	return bd.OutputWriter.Close()
}

func formatJSON(doc *bson.Raw, pretty bool) ([]byte, error) {
	extendedJSON, err := bson.MarshalExtJSON(doc, true, false)
	if err != nil {
		return nil, fmt.Errorf("error converting BSON to extended JSON: %v", err)
	}

	if pretty {
		var jsonFormatted bytes.Buffer
		if err := json.Indent(&jsonFormatted, extendedJSON, "", "\t"); err != nil {
			return nil, fmt.Errorf("error prettifying extended JSON: %v", err)
		}
		extendedJSON = jsonFormatted.Bytes()
	}
	return extendedJSON, nil
}

// JSON iterates through the BSON file and for each document it finds,
// recursively descends into objects and arrays and prints the human readable
// JSON representation.
// It returns the number of documents processed and a non-nil error if one is
// encountered before the end of the file is reached.
func (bd *BSONDump) JSON() (int, error) {
	numFound := 0

	if bd.InputSource == nil {
		panic("Tried to call JSON() before opening file")
	}

	for {
		result := bson.Raw(bd.InputSource.LoadNext())
		if result == nil {
			break
		}

		if bytes, err := formatJSON(&result, bd.OutputOptions.Pretty); err != nil {
			log.Logvf(log.Always, "unable to dump document %v: %v", numFound+1, err)

			//if objcheck is turned on, stop now. otherwise keep on dumpin'
			if bd.OutputOptions.ObjCheck {
				return numFound, err
			}
		} else {
			bytes = append(bytes, '\n')
			_, err := bd.OutputWriter.Write(bytes)
			if err != nil {
				return numFound, err
			}
		}
		numFound++
		if failpoint.Enabled(failpoint.SlowBSONDump) {
			time.Sleep(2 * time.Second)
		}
	}
	if err := bd.InputSource.Err(); err != nil {
		return numFound, err
	}

	return numFound, nil
}

// Debug iterates through the BSON file and for each document it finds,
// recursively descends into objects and arrays and prints a human readable
// BSON representation containing the type and size of each field.
// It returns the number of documents processed and a non-nil error if one is
// encountered before the end of the file is reached.
func (bd *BSONDump) Debug() (int, error) {
	numFound := 0

	if bd.InputSource == nil {
		panic("Tried to call Debug() before opening file")
	}

	for {
		result := bson.Raw(bd.InputSource.LoadNext())
		if result == nil {
			break
		}

		if bd.OutputOptions.ObjCheck {
			validated := bson.M{}
			err := bson.Unmarshal(result, &validated)
			if err != nil {
				// ObjCheck is turned on and we hit an error, so short-circuit now.
				return numFound, fmt.Errorf("failed to validate bson during objcheck: %v", err)
			}
		}
		err := printBSON(result, 0, bd.OutputWriter)
		if err != nil {
			log.Logvf(log.Always, "encountered error debugging BSON data: %v", err)
		}
		numFound++
	}

	if err := bd.InputSource.Err(); err != nil {
		// This error indicates the BSON document header is corrupted;
		// either the 4-byte header couldn't be read in full, or
		// the size in the header would require reading more bytes
		// than the file has left
		return numFound, err
	}
	return numFound, nil
}

func printBSON(raw bson.Raw, indentLevel int, out io.Writer) error {
	indent := strings.Repeat("\t", indentLevel)
	fmt.Fprintf(out, "%v--- new object ---\n", indent)
	fmt.Fprintf(out, "%v\tsize : %v\n", indent, len(raw))

	elements, err := raw.Elements()
	if err != nil {
		return err
	}
	for _, rawElem := range elements {
		key := rawElem.Key()
		value := rawElem.Value()

		fmt.Fprintf(out, "%v\t\t%v\n", indent, key)

		// the size of an element is the combined size of the following:
		// 1. 1 byte for the BSON type
		// 2. 'e_name' : the BSON key, which is a null-terminated cstring
		// 3. The BSON value
		// So size == 1 [size of type byte] +  1 [null byte for cstring key] + len(bson key) + len(bson value)
		// see http://bsonspec.org/spec.html for more details
		fmt.Fprintf(out, "%v\t\t\ttype: %4v size: %v\n", indent, int8(value.Type), len(rawElem))

		//For nested objects or arrays, recurse.
		if value.Type == bsontype.EmbeddedDocument || value.Type == bsontype.Array {
			err = printBSON(value.Value, indentLevel+3, out)
			if err != nil {
				return err
			}
		}
	}
	return nil
}
