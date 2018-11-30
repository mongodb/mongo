// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

// Usage describes basic usage of mongorestore
var Usage = `<options> <directory or file to restore>

Restore backups generated with mongodump to a running server.

Specify a database with -d to restore a single database from the target directory,
or use -d and -c to restore a single collection from a single .bson file.

See http://docs.mongodb.org/manual/reference/program/mongorestore/ for more information.`

// InputOptions defines the set of options to use in configuring the restore process.
type InputOptions struct {
	Objcheck               bool   `long:"objcheck" description:"validate all objects before inserting"`
	OplogReplay            bool   `long:"oplogReplay" description:"replay oplog for point-in-time restore"`
	OplogLimit             string `long:"oplogLimit" value-name:"<seconds>[:ordinal]" description:"only include oplog entries before the provided Timestamp"`
	OplogFile              string `long:"oplogFile" value-name:"<filename>" description:"oplog file to use for replay of oplog"`
	Archive                string `long:"archive" value-name:"<filename>" optional:"true" optional-value:"-" description:"restore dump from the specified archive file.  If flag is specified without a value, archive is read from stdin"`
	RestoreDBUsersAndRoles bool   `long:"restoreDbUsersAndRoles" description:"restore user and role definitions for the given database"`
	Directory              string `long:"dir" value-name:"<directory-name>" description:"input directory, use '-' for stdin"`
	Gzip                   bool   `long:"gzip" description:"decompress gzipped input"`
}

// Name returns a human-readable group name for input options.
func (*InputOptions) Name() string {
	return "input"
}

// OutputOptions defines the set of options for restoring dump data.
type OutputOptions struct {
	Drop   bool `long:"drop" description:"drop each collection before import"`
	DryRun bool `long:"dryRun" description:"view summary without importing anything. recommended with verbosity"`

	// By default mongorestore uses a write concern of 'majority'.
	// Cannot be used simultaneously with write concern options in a URI.
	WriteConcern             string `long:"writeConcern" value-name:"<write-concern>" default-mask:"-" description:"write concern options e.g. --writeConcern majority, --writeConcern '{w: 3, wtimeout: 500, fsync: true, j: true}'"`
	NoIndexRestore           bool   `long:"noIndexRestore" description:"don't restore indexes"`
	NoOptionsRestore         bool   `long:"noOptionsRestore" description:"don't restore collection options"`
	KeepIndexVersion         bool   `long:"keepIndexVersion" description:"don't update index version"`
	MaintainInsertionOrder   bool   `long:"maintainInsertionOrder" description:"preserve order of documents during restoration"`
	NumParallelCollections   int    `long:"numParallelCollections" short:"j" description:"number of collections to restore in parallel (4 by default)" default:"4" default-mask:"-"`
	NumInsertionWorkers      int    `long:"numInsertionWorkersPerCollection" description:"number of insert operations to run concurrently per collection (1 by default)" default:"1" default-mask:"-"`
	StopOnError              bool   `long:"stopOnError" description:"stop restoring if an error is encountered on insert (off by default)"`
	BypassDocumentValidation bool   `long:"bypassDocumentValidation" description:"bypass document validation"`
	PreserveUUID             bool   `long:"preserveUUID" description:"preserve original collection UUIDs (off by default, requires drop)"`
	TempUsersColl            string `long:"tempUsersColl" default:"tempusers" hidden:"true"`
	TempRolesColl            string `long:"tempRolesColl" default:"temproles" hidden:"true"`
	BulkBufferSize           int    `long:"batchSize" default:"1000" hidden:"true"`
}

// Name returns a human-readable group name for output options.
func (*OutputOptions) Name() string {
	return "restore"
}

// NSOptions defines the set of options for configuring involved namespaces
type NSOptions struct {
	DB                         string   `short:"d" long:"db" value-name:"<database-name>" description:"database to use when restoring from a BSON file"`
	Collection                 string   `short:"c" long:"collection" value-name:"<collection-name>" description:"collection to use when restoring from a BSON file"`
	ExcludedCollections        []string `long:"excludeCollection" value-name:"<collection-name>" description:"DEPRECATED; collection to skip over during restore (may be specified multiple times to exclude additional collections)"`
	ExcludedCollectionPrefixes []string `long:"excludeCollectionsWithPrefix" value-name:"<collection-prefix>" description:"DEPRECATED; collections to skip over during restore that have the given prefix (may be specified multiple times to exclude additional prefixes)"`
	NSExclude                  []string `long:"nsExclude" value-name:"<namespace-pattern>" description:"exclude matching namespaces"`
	NSInclude                  []string `long:"nsInclude" value-name:"<namespace-pattern>" description:"include matching namespaces"`
	NSFrom                     []string `long:"nsFrom" value-name:"<namespace-pattern>" description:"rename matching namespaces, must have matching nsTo"`
	NSTo                       []string `long:"nsTo" value-name:"<namespace-pattern>" description:"rename matched namespaces, must have matching nsFrom"`
}

// Name returns a human-readable group name for output options.
func (*NSOptions) Name() string {
	return "namespace"
}
