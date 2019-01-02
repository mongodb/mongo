// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongofiles

var Usage = `<options> <command> <filename or _id>

Manipulate gridfs files using the command line.

Possible commands include:
	list      - list all files; 'filename' is an optional prefix which listed filenames must begin with
	search    - search all files; 'filename' is a substring which listed filenames must contain
	put       - add a file with filename 'filename'
	put_id    - add a file with filename 'filename' and a given '_id'
	get       - get a file with filename 'filename'
	get_id    - get a file with the given '_id'
	delete    - delete all files with filename 'filename'
	delete_id - delete a file with the given '_id'

See http://docs.mongodb.org/manual/reference/program/mongofiles/ for more information.`

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
func (_ *StorageOptions) Name() string {
	return "storage"
}

// InputOptions defines the set of options to use in retrieving data from the server.
type InputOptions struct {
	ReadPreference string `long:"readPreference" value-name:"<string>|<json>" description:"specify either a preference name or a preference json object"`
}

// Name returns a human-readable group name for input options.
func (*InputOptions) Name() string {
	return "query"
}
