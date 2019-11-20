// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongorestore

import (
	"github.com/mongodb/mongo-tools-common/db"
	"github.com/mongodb/mongo-tools-common/log"
	"github.com/mongodb/mongo-tools-common/options"
	"github.com/mongodb/mongo-tools-common/util"

	"fmt"
)

// Usage describes basic usage of mongorestore
var Usage = `<options> <directory or file to restore>

Restore backups generated with mongodump to a running server.

Specify a database with -d to restore a single database from the target directory,
or use -d and -c to restore a single collection from a single .bson file.

See http://docs.mongodb.org/manual/reference/program/mongorestore/ for more information.`

// Options defines the set of all options for configuring mongorestore.
type Options struct {
	*options.ToolOptions
	*InputOptions
	*NSOptions
	*OutputOptions
	TargetDirectory string
}

// InputOptions command line argument long names
const (
	ObjcheckOption               = "--objcheck"
	OplogReplayOption            = "--oplogReplay"
	OplogLimitOption             = "--oplogLimit"
	OplogFileOption              = "--oplogFile"
	ArchiveOption                = "--archive" // Value is optional, so must use '=' if specifying one
	RestoreDBUsersAndRolesOption = "--restoreDbUsersAndRoles"
	DirectoryOption              = "--dir"
	GzipOption                   = "--gzip"
)

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

// OutputOptions command line argument long names
const (
	DropOption                     = "--drop"
	DryRunOption                   = "--dryRun"
	WriteConcernOption             = "--writeConcern"
	NoIndexRestoreOption           = "--noIndexRestore"
	ConvertLegacyIndexesOption     = "--convertLegacyIndexes"
	NoOptionsRestoreOption         = "--noOptionsRestore"
	KeepIndexVersionOption         = "--keepIndexVersion"
	MaintainInsertionOrderOption   = "--maintainInsertionOrder"
	NumParallelCollectionsOption   = "--numParallelCollections"
	NumInsertionWorkersOption      = "--numInsertionWorkersPerCollection"
	StopOnErrorOption              = "--stopOnError"
	BypassDocumentValidationOption = "--bypassDocumentValidation"
	PreserveUUIDOption             = "--preserveUUID"
	TempUsersCollOption            = "--tempUsersColl"
	TempRolesCollOption            = "--tempRolesColl"
	BulkBufferSizeOption           = "--batchSize"
)

// OutputOptions defines the set of options for restoring dump data.
type OutputOptions struct {
	Drop   bool `long:"drop" description:"drop each collection before import"`
	DryRun bool `long:"dryRun" description:"view summary without importing anything. recommended with verbosity"`

	// By default mongorestore uses a write concern of 'majority'.
	WriteConcern             string `long:"writeConcern" value-name:"<write-concern>" default-mask:"-" description:"write concern options e.g. --writeConcern majority, --writeConcern '{w: 3, wtimeout: 500, fsync: true, j: true}'"`
	NoIndexRestore           bool   `long:"noIndexRestore" description:"don't restore indexes"`
	ConvertLegacyIndexes     bool   `long:"convertLegacyIndexes" description:"Removes invalid index options and rewrites legacy option values (e.g. true becomes 1)."`
	NoOptionsRestore         bool   `long:"noOptionsRestore" description:"don't restore collection options"`
	KeepIndexVersion         bool   `long:"keepIndexVersion" description:"don't update index version"`
	MaintainInsertionOrder   bool   `long:"maintainInsertionOrder" description:"restore the documents in the order of their appearance in the input source. By default the insertions will be performed in an arbitrary order. Setting this flag also enables the behavior of --stopOnError and restricts NumInsertionWorkersPerCollection to 1."`
	NumParallelCollections   int    `long:"numParallelCollections" short:"j" description:"number of collections to restore in parallel (4 by default)" default:"4" default-mask:"-"`
	NumInsertionWorkers      int    `long:"numInsertionWorkersPerCollection" description:"number of insert operations to run concurrently per collection (1 by default)" default:"1" default-mask:"-"`
	StopOnError              bool   `long:"stopOnError" description:"halt after encountering any error during insertion. By default, mongorestore will attempt to continue through document validation and DuplicateKey errors, but with this option enabled, the tool will stop instead. A small number of documents may be inserted after encountering an error even with this option enabled; use --maintainInsertionOrder to halt immediately after an error"`
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

// NSOptions command line argument long names
const (
	DBOption                         = "--db"
	CollectionOption                 = "--collection"
	ExcludedCollectionsOption        = "--excludeCollection"
	ExcludedCollectionPrefixesOption = "--excludeCollectionsWithPrefix"
	NSExcludeOption                  = "--nsExclude"
	NSIncludeOption                  = "--nsInclude"
	NSFromOption                     = "--nsFrom"
	NSToOption                       = "--nsTo"
)

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

// ParseOptions reads the command line arguments and converts them into options used to configure a MongoRestore instance
func ParseOptions(rawArgs []string, versionStr, gitCommit string) (Options, error) {
	opts := options.New("mongorestore", versionStr, gitCommit, Usage,
		options.EnabledOptions{Auth: true, Connection: true, URI: true})
	nsOpts := &NSOptions{}
	opts.AddOptions(nsOpts)

	inputOpts := &InputOptions{}
	opts.AddOptions(inputOpts)

	outputOpts := &OutputOptions{}
	opts.AddOptions(outputOpts)

	opts.URI.AddKnownURIParameters(options.KnownURIOptionsWriteConcern)

	extraArgs, err := opts.ParseArgs(rawArgs)
	if err != nil {
		return Options{}, fmt.Errorf("error parsing command line options: %v", err)
	}

	// Allow the db connector to fall back onto the current database when no
	// auth database is given; the standard -d/-c options go into nsOpts now
	opts.Namespace = &options.Namespace{DB: nsOpts.DB}

	log.SetVerbosity(opts.Verbosity)

	// verify uri options and log them
	opts.URI.LogUnsupportedOptions()

	targetDir, err := getTargetDirFromArgs(extraArgs, inputOpts.Directory)
	if err != nil {
		return Options{}, fmt.Errorf("error parsing target dir: %v", err)
	}
	targetDir = util.ToUniversalPath(targetDir)

	wc, err := db.NewMongoWriteConcern(outputOpts.WriteConcern, opts.URI.ParsedConnString())
	if err != nil {
		return Options{}, fmt.Errorf("error parsing write concern: %v", err)
	}
	opts.WriteConcern = wc

	return Options{opts, inputOpts, nsOpts, outputOpts, targetDir}, nil
}

// getTargetDirFromArgs handles the logic and error cases of figuring out
// the target restore directory.
func getTargetDirFromArgs(extraArgs []string, dirFlag string) (string, error) {
	// This logic is in a switch statement so that the rules are understandable.
	// We start by handling error cases, and then handle the different ways the target
	// directory can be legally set.
	switch {
	case len(extraArgs) > 1:
		// error on cases when there are too many positional arguments
		return "", fmt.Errorf("too many positional arguments")

	case dirFlag != "" && len(extraArgs) > 0:
		// error when positional arguments and --dir are used
		return "", fmt.Errorf(
			"cannot use both %v and a positional argument to set the target directory", DirectoryOption)

	case len(extraArgs) == 1:
		// a nice, simple case where one argument is given, so we use it
		return extraArgs[0], nil

	case dirFlag != "":
		// if we have no extra args and a --dir flag, use the --dir flag
		log.Logv(log.Info, "using "+DirectoryOption+" flag instead of arguments")
		return dirFlag, nil

	default:
		return "", nil
	}
}
