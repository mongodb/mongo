// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoexport

import (
	"fmt"
	"io/ioutil"
)

var Usage = `<options>

Export data from MongoDB in CSV or JSON format.

See http://docs.mongodb.org/manual/reference/program/mongoexport/ for more information.`

// OutputFormatOptions defines the set of options to use in formatting exported data.
type OutputFormatOptions struct {
	// Fields is an option to directly specify comma-separated fields to export to CSV.
	Fields string `long:"fields" value-name:"<field>[,<field>]*" short:"f" description:"comma separated list of field names (required for exporting CSV) e.g. -f \"name,age\" "`

	// FieldFile is a filename that refers to a list of fields to export, 1 per line.
	FieldFile string `long:"fieldFile" value-name:"<filename>" description:"file with field names - 1 per line"`

	// Type selects the type of output to export as (json or csv).
	Type string `long:"type" value-name:"<type>" default:"json" default-mask:"-" description:"the output format, either json or csv (defaults to 'json')"`

	// Deprecated: allow legacy --csv option in place of --type=csv
	CSVOutputType bool `long:"csv" default:"false" hidden:"true"`

	// OutputFile specifies an output file path.
	OutputFile string `long:"out" value-name:"<filename>" short:"o" description:"output file; if not specified, stdout is used"`

	// JSONArray if set will export the documents an array of JSON documents.
	JSONArray bool `long:"jsonArray" description:"output to a JSON array rather than one object per line"`

	// Pretty displays JSON data in a human-readable form.
	Pretty bool `long:"pretty" description:"output JSON formatted to be human-readable"`

	// NoHeaderLine, if set, will export CSV data without a list of field names at the first line.
	NoHeaderLine bool `long:"noHeaderLine" description:"export CSV data without a list of field names at the first line"`
}

// Name returns a human-readable group name for output format options.
func (*OutputFormatOptions) Name() string {
	return "output"
}

// InputOptions defines the set of options to use in retrieving data from the server.
type InputOptions struct {
	Query          string `long:"query" value-name:"<json>" short:"q" description:"query filter, as a JSON string, e.g., '{x:{$gt:1}}'"`
	QueryFile      string `long:"queryFile" value-name:"<filename>" description:"path to a file containing a query filter (JSON)"`
	SlaveOk        bool   `long:"slaveOk" short:"k" description:"allow secondary reads if available (default true)" default:"false" default-mask:"-"`
	ReadPreference string `long:"readPreference" value-name:"<string>|<json>" description:"specify either a preference name or a preference json object"`
	ForceTableScan bool   `long:"forceTableScan" description:"force a table scan (do not use $snapshot)"`
	Skip           int    `long:"skip" value-name:"<count>" description:"number of documents to skip"`
	Limit          int    `long:"limit" value-name:"<count>" description:"limit the number of documents to export"`
	Sort           string `long:"sort" value-name:"<json>" description:"sort order, as a JSON string, e.g. '{x:1}'"`
	AssertExists   bool   `long:"assertExists" default:"false" description:"if specified, export fails if the collection does not exist"`
}

// Name returns a human-readable group name for input options.
func (*InputOptions) Name() string {
	return "querying"
}

func (inputOptions *InputOptions) HasQuery() bool {
	return inputOptions.Query != "" || inputOptions.QueryFile != ""
}

func (inputOptions *InputOptions) GetQuery() ([]byte, error) {
	if inputOptions.Query != "" {
		return []byte(inputOptions.Query), nil
	} else if inputOptions.QueryFile != "" {
		content, err := ioutil.ReadFile(inputOptions.QueryFile)
		if err != nil {
			err = fmt.Errorf("error reading queryFile: %s", err)
		}
		return content, err
	}
	panic("GetQuery can return valid values only for query or queryFile input")
}
