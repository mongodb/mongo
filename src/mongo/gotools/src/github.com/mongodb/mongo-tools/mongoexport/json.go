// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoexport

import (
	"bytes"
	"io"

	"github.com/mongodb/mongo-tools-common/json"
	"go.mongodb.org/mongo-driver/bson"
)

// JSONExportOutput is an implementation of ExportOutput that writes documents
// to the output in JSON format.
type JSONExportOutput struct {
	// ArrayOutput when set to true indicates that the output should be written
	// as a JSON array, where each document is an element in the array.
	ArrayOutput bool
	// Pretty when set to true indicates that the output will be written in pretty mode.
	PrettyOutput bool
	Out          io.Writer
	NumExported  int64
	JSONFormat   JSONFormat
}

// NewJSONExportOutput creates a new JSONExportOutput in array mode if specified,
// configured to write data to the given io.Writer.
func NewJSONExportOutput(arrayOutput bool, prettyOutput bool, out io.Writer, jsonFormat JSONFormat) *JSONExportOutput {
	return &JSONExportOutput{
		arrayOutput,
		prettyOutput,
		out,
		0,
		jsonFormat,
	}
}

// WriteHeader writes the opening square bracket if in array mode, otherwise it
// behaves as a no-op.
func (jsonExporter *JSONExportOutput) WriteHeader() error {
	if jsonExporter.ArrayOutput {
		// TODO check # bytes written?
		_, err := jsonExporter.Out.Write([]byte{json.ArrayStart})
		if err != nil {
			return err
		}
	}
	return nil
}

// WriteFooter writes the closing square bracket if in array mode, otherwise it
// behaves as a no-op.
func (jsonExporter *JSONExportOutput) WriteFooter() error {
	if jsonExporter.ArrayOutput {
		_, err := jsonExporter.Out.Write([]byte{json.ArrayEnd, '\n'})
		// TODO check # bytes written?
		if err != nil {
			return err
		}
	}
	if jsonExporter.PrettyOutput {
		if _, err := jsonExporter.Out.Write([]byte("\n")); err != nil {
			return err
		}
	}
	return nil
}

// Flush is a no-op for JSON export formats.
func (jsonExporter *JSONExportOutput) Flush() error {
	return nil
}

// ExportDocument converts the given document to extended JSON, and writes it
// to the output.
func (jsonExporter *JSONExportOutput) ExportDocument(document bson.D) error {
	if jsonExporter.ArrayOutput || jsonExporter.PrettyOutput {
		if jsonExporter.NumExported >= 1 {
			if jsonExporter.ArrayOutput {
				if _, err := jsonExporter.Out.Write([]byte(",")); err != nil {
					return err
				}
			}
			if jsonExporter.PrettyOutput {
				if _, err := jsonExporter.Out.Write([]byte("\n")); err != nil {
					return err
				}
			}
		}

		jsonOut, err := bson.MarshalExtJSON(document, jsonExporter.JSONFormat == Canonical, false)
		if err != nil {
			return err
		}

		if jsonExporter.PrettyOutput {
			var jsonFormatted bytes.Buffer
			if err = json.Indent(&jsonFormatted, jsonOut, "", "\t"); err != nil {
				return err
			}
			jsonOut = jsonFormatted.Bytes()
		}

		if _, err = jsonExporter.Out.Write(jsonOut); err != nil {
			return err
		}
	} else {
		extendedDoc, err := bson.MarshalExtJSON(document, jsonExporter.JSONFormat == Canonical, false)
		if err != nil {
			return err
		}

		extendedDoc = append(extendedDoc, '\n')
		if _, err = jsonExporter.Out.Write(extendedDoc); err != nil {
			return err
		}
	}
	jsonExporter.NumExported++
	return nil
}
