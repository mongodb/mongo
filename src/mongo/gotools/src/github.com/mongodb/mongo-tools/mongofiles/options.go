// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongofiles

import (
	"fmt"

	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/options"
)

// Usage string printed as part of --help
var Usage = `<options> <command> <filename or _id>

Manipulate gridfs files using the command line.

Possible commands include:
	list      - list all files; 'filename' is an optional prefix which listed filenames must begin with
	search    - search all files; 'filename' is a regex which listed filenames must match
	put       - add a file with filename 'filename'
	put_id    - add a file with filename 'filename' and a given '_id'
	get       - get a file with filename 'filename'
	get_id    - get a file with the given '_id'
	delete    - delete all files with filename 'filename'
	delete_id - delete a file with the given '_id'

See http://docs.mongodb.org/manual/reference/program/mongofiles/ for more information.`

// ParseOptions reads command line arguments and converts them into options used to configure a MongoFiles instance
func ParseOptions(rawArgs []string, versionStr, gitCommit string) (Options, error) {
	// initialize command-line opts
	opts := options.New("mongofiles", versionStr, gitCommit, Usage, options.EnabledOptions{Auth: true, Connection: true, Namespace: false, URI: true})

	storageOpts := &StorageOptions{}
	inputOpts := &InputOptions{}

	opts.AddOptions(storageOpts)
	opts.AddOptions(inputOpts)
	opts.URI.AddKnownURIParameters(options.KnownURIOptionsReadPreference)
	opts.URI.AddKnownURIParameters(options.KnownURIOptionsWriteConcern)

	args, err := opts.ParseArgs(rawArgs)
	if err != nil {
		return Options{}, fmt.Errorf("error parsing command line options: %v", err)
	}

	log.SetVerbosity(opts.Verbosity)

	// verify uri options and log them
	opts.URI.LogUnsupportedOptions()

	// add the specified database to the namespace options struct
	opts.Namespace.DB = storageOpts.DB

	// set WriteConcern
	wc, err := db.NewMongoWriteConcern(storageOpts.WriteConcern, opts.URI.ParsedConnString())
	if err != nil {
		return Options{}, fmt.Errorf("error parsing --writeConcern: %v", err)
	}
	opts.WriteConcern = wc

	// set ReadPreference
	opts.ReadPreference, err = db.NewReadPreference(inputOpts.ReadPreference, opts.URI.ParsedConnString())
	if err != nil {
		return Options{}, fmt.Errorf("error parsing --readPreference: %v", err)
	}

	return Options{opts, storageOpts, inputOpts, args}, nil
}

// Options contains all the possible options that can configure mongofiles
type Options struct {
	*options.ToolOptions
	*StorageOptions
	*InputOptions
	ParsedArgs []string
}

// StorageOptions defines the set of options to use in storing/retrieving data from server.
type StorageOptions struct {
	// Specified database to use. defaults to 'test' if none is specified
	DB string `short:"d" value-name:"<database-name>" default:"test" default-mask:"-" long:"db" description:"database to use (default is 'test')"`

	// 'LocalFileName' is an option that specifies what filename to use for (put|get)
	LocalFileName string `long:"local" value-name:"<filename>" short:"l" description:"local filename for put|get"`

	// 'ContentType' is an option that specifies the Content/MIME type to use for 'put'
	ContentType string `long:"type" value-nane:"<content-type>" short:"t" description:"content/MIME type for put (optional)"`

	// if set, 'Replace' will remove other files with same name after 'put'
	Replace bool `long:"replace" short:"r" description:"remove other files with same name after put"`

	// GridFSPrefix specifies what GridFS prefix to use; defaults to 'fs'
	GridFSPrefix string `long:"prefix" value-name:"<prefix>" default:"fs" default-mask:"-" description:"GridFS prefix to use (default is 'fs')"`

	// Specifies the write concern for each write operation that mongofiles writes to the target database.
	// By default, mongofiles waits for a majority of members from the replica set to respond before returning.
	// Cannot be used simultaneously with write concern options in a URI.
	WriteConcern string `long:"writeConcern" value-name:"<write-concern>" default-mask:"-" description:"write concern options e.g. --writeConcern majority, --writeConcern '{w: 3, wtimeout: 500, fsync: true, j: true}'"`
}

// Name returns a human-readable group name for storage options.
func (*StorageOptions) Name() string {
	return "storage"
}

// InputOptions defines the set of options to use in retrieving data from the server.
type InputOptions struct {
	ReadPreference string `long:"readPreference" value-name:"<string>|<json>" description:"specify either a preference mode (e.g. 'nearest') or a preference json object (e.g. '{mode: \"nearest\", tagSets: [{a: \"b\"}], maxStalenessSeconds: 123}')"`
}

// Name returns a human-readable group name for input options.
func (*InputOptions) Name() string {
	return "query"
}
