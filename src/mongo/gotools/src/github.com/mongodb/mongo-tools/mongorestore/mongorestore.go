// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package mongorestore writes BSON data to a MongoDB instance.
package mongorestore

import (
	"compress/gzip"
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"path/filepath"
	"sync"

	"github.com/mongodb/mongo-tools/common/archive"
	"github.com/mongodb/mongo-tools/common/auth"
	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/progress"
	"github.com/mongodb/mongo-tools/common/util"
	"github.com/mongodb/mongo-tools/mongorestore/ns"
	"gopkg.in/mgo.v2"
	"gopkg.in/mgo.v2/bson"
)

// MongoRestore is a container for the user-specified options and
// internal state used for running mongorestore.
type MongoRestore struct {
	ToolOptions   *options.ToolOptions
	InputOptions  *InputOptions
	OutputOptions *OutputOptions
	NSOptions     *NSOptions

	SessionProvider *db.SessionProvider
	ProgressManager progress.Manager

	TargetDirectory string

	// Skip restoring users and roles, regardless of namespace, when true.
	SkipUsersAndRoles bool

	// other internal state
	manager *intents.Manager
	safety  *mgo.Safe

	objCheck         bool
	oplogLimit       bson.MongoTimestamp
	isMongos         bool
	useWriteCommands bool
	authVersions     authVersionPair

	// a map of database names to a list of collection names
	knownCollections      map[string][]string
	knownCollectionsMutex sync.Mutex

	renamer  *ns.Renamer
	includer *ns.Matcher
	excluder *ns.Matcher

	// indexes belonging to dbs and collections
	dbCollectionIndexes map[string]collectionIndexes

	archive *archive.Reader

	// channel on which to notify if/when a termination signal is received
	termChan chan struct{}

	// Reader to take care of BSON input if not reading from the local filesystem.
	// This is initialized to os.Stdin if unset.
	InputReader io.Reader
}

type collectionIndexes map[string][]IndexDocument

// ParseAndValidateOptions returns a non-nil error if user-supplied options are invalid.
func (restore *MongoRestore) ParseAndValidateOptions() error {
	// Can't use option pkg defaults for --objcheck because it's two separate flags,
	// and we need to be able to see if they're both being used. We default to
	// true here and then see if noobjcheck is enabled.
	log.Logv(log.DebugHigh, "checking options")
	if restore.InputOptions.Objcheck {
		restore.objCheck = true
		log.Logv(log.DebugHigh, "\tdumping with object check enabled")
	} else {
		log.Logv(log.DebugHigh, "\tdumping with object check disabled")
	}

	if restore.NSOptions.DB == "" && restore.NSOptions.Collection != "" {
		return fmt.Errorf("cannot restore a collection without a specified database")
	}

	if restore.NSOptions.DB != "" {
		if err := util.ValidateDBName(restore.NSOptions.DB); err != nil {
			return fmt.Errorf("invalid db name: %v", err)
		}
	}
	if restore.NSOptions.Collection != "" {
		if err := util.ValidateCollectionGrammar(restore.NSOptions.Collection); err != nil {
			return fmt.Errorf("invalid collection name: %v", err)
		}
	}
	if restore.InputOptions.RestoreDBUsersAndRoles && restore.NSOptions.DB == "" {
		return fmt.Errorf("cannot use --restoreDbUsersAndRoles without a specified database")
	}
	if restore.InputOptions.RestoreDBUsersAndRoles && restore.NSOptions.DB == "admin" {
		return fmt.Errorf("cannot use --restoreDbUsersAndRoles with the admin database")
	}

	var err error
	restore.isMongos, err = restore.SessionProvider.IsMongos()
	if err != nil {
		return err
	}
	if restore.isMongos {
		log.Logv(log.DebugLow, "restoring to a sharded system")
	}

	if restore.InputOptions.OplogLimit != "" {
		if !restore.InputOptions.OplogReplay {
			return fmt.Errorf("cannot use --oplogLimit without --oplogReplay enabled")
		}
		restore.oplogLimit, err = ParseTimestampFlag(restore.InputOptions.OplogLimit)
		if err != nil {
			return fmt.Errorf("error parsing timestamp argument to --oplogLimit: %v", err)
		}
	}
	if restore.InputOptions.OplogFile != "" {
		if !restore.InputOptions.OplogReplay {
			return fmt.Errorf("cannot use --oplogFile without --oplogReplay enabled")
		}
		if restore.InputOptions.Archive != "" {
			return fmt.Errorf("cannot use --oplogFile with --archive specified")
		}
	}

	// check if we are using a replica set and fall back to w=1 if we aren't (for <= 2.4)
	nodeType, err := restore.SessionProvider.GetNodeType()
	if err != nil {
		return fmt.Errorf("error determining type of connected node: %v", err)
	}

	log.Logvf(log.DebugLow, "connected to node type: %v", nodeType)
	restore.safety, err = db.BuildWriteConcern(restore.OutputOptions.WriteConcern, nodeType,
		restore.ToolOptions.URI.ParsedConnString())
	if err != nil {
		return fmt.Errorf("error parsing write concern: %v", err)
	}

	// deprecations with --nsInclude --nsExclude
	if restore.NSOptions.DB != "" || restore.NSOptions.Collection != "" {
		// these are only okay if restoring from a bson file
		_, fileType := restore.getInfoFromFilename(restore.TargetDirectory)
		if fileType != BSONFileType {
			log.Logvf(log.Always, "the --db and --collection args should only be used when "+
				"restoring from a BSON file. Other uses are deprecated and will not exist "+
				"in the future; use --nsInclude instead")
		}
	}
	if len(restore.NSOptions.ExcludedCollections) > 0 ||
		len(restore.NSOptions.ExcludedCollectionPrefixes) > 0 {
		log.Logvf(log.Always, "the --excludeCollections and --excludeCollectionPrefixes options "+
			"are deprecated and will not exist in the future; use --nsExclude instead")
	}
	if restore.InputOptions.OplogReplay {
		if len(restore.NSOptions.NSInclude) > 0 || restore.NSOptions.DB != "" {
			return fmt.Errorf("cannot use --oplogReplay with includes specified")
		}
		if len(restore.NSOptions.NSExclude) > 0 || len(restore.NSOptions.ExcludedCollections) > 0 ||
			len(restore.NSOptions.ExcludedCollectionPrefixes) > 0 {
			return fmt.Errorf("cannot use --oplogReplay with excludes specified")
		}
		if len(restore.NSOptions.NSFrom) > 0 {
			return fmt.Errorf("cannot use --oplogReplay with namespace renames specified")
		}
	}

	includes := restore.NSOptions.NSInclude
	if restore.NSOptions.DB != "" && restore.NSOptions.Collection != "" {
		includes = append(includes, ns.Escape(restore.NSOptions.DB)+"."+
			restore.NSOptions.Collection)
	} else if restore.NSOptions.DB != "" {
		includes = append(includes, ns.Escape(restore.NSOptions.DB)+".*")
	}
	if len(includes) == 0 {
		includes = []string{"*"}
	}
	restore.includer, err = ns.NewMatcher(includes)
	if err != nil {
		return fmt.Errorf("invalid includes: %v", err)
	}

	if len(restore.NSOptions.ExcludedCollections) > 0 && restore.NSOptions.Collection != "" {
		return fmt.Errorf("--collection is not allowed when --excludeCollection is specified")
	}
	if len(restore.NSOptions.ExcludedCollectionPrefixes) > 0 && restore.NSOptions.Collection != "" {
		return fmt.Errorf("--collection is not allowed when --excludeCollectionsWithPrefix is specified")
	}
	excludes := restore.NSOptions.NSExclude
	for _, col := range restore.NSOptions.ExcludedCollections {
		excludes = append(excludes, "*."+ns.Escape(col))
	}
	for _, colPrefix := range restore.NSOptions.ExcludedCollectionPrefixes {
		excludes = append(excludes, "*."+ns.Escape(colPrefix)+"*")
	}
	restore.excluder, err = ns.NewMatcher(excludes)
	if err != nil {
		return fmt.Errorf("invalid excludes: %v", err)
	}

	if len(restore.NSOptions.NSFrom) != len(restore.NSOptions.NSTo) {
		return fmt.Errorf("--nsFrom and --nsTo arguments must be specified an equal number of times")
	}
	restore.renamer, err = ns.NewRenamer(restore.NSOptions.NSFrom, restore.NSOptions.NSTo)
	if err != nil {
		return fmt.Errorf("invalid renames: %v", err)
	}

	if restore.OutputOptions.NumInsertionWorkers < 0 {
		return fmt.Errorf(
			"cannot specify a negative number of insertion workers per collection")
	}

	// a single dash signals reading from stdin
	if restore.TargetDirectory == "-" {
		if restore.InputOptions.Archive != "" {
			return fmt.Errorf(
				"cannot restore from \"-\" when --archive is specified")
		}
		if restore.NSOptions.Collection == "" {
			return fmt.Errorf("cannot restore from stdin without a specified collection")
		}
	}
	if restore.InputReader == nil {
		restore.InputReader = os.Stdin
	}

	return nil
}

// Restore runs the mongorestore program.
func (restore *MongoRestore) Restore() error {
	var target archive.DirLike
	err := restore.ParseAndValidateOptions()
	if err != nil {
		log.Logvf(log.DebugLow, "got error from options parsing: %v", err)
		return err
	}

	// Build up all intents to be restored
	restore.manager = intents.NewIntentManager()
	if restore.InputOptions.Archive == "" && restore.InputOptions.OplogReplay {
		restore.manager.SetSmartPickOplog(true)
	}

	if restore.InputOptions.Archive != "" {
		if restore.archive == nil {
			archiveReader, err := restore.getArchiveReader()
			if err != nil {
				return err
			}
			restore.archive = &archive.Reader{
				In:      archiveReader,
				Prelude: &archive.Prelude{},
			}
		}
		err = restore.archive.Prelude.Read(restore.archive.In)
		if err != nil {
			return err
		}
		log.Logvf(log.DebugLow, `archive format version "%v"`, restore.archive.Prelude.Header.FormatVersion)
		log.Logvf(log.DebugLow, `archive server version "%v"`, restore.archive.Prelude.Header.ServerVersion)
		log.Logvf(log.DebugLow, `archive tool version "%v"`, restore.archive.Prelude.Header.ToolVersion)
		target, err = restore.archive.Prelude.NewPreludeExplorer()
		if err != nil {
			return err
		}
	} else if restore.TargetDirectory != "-" {
		var usedDefaultTarget bool
		if restore.TargetDirectory == "" {
			restore.TargetDirectory = "dump"
			log.Logv(log.Always, "using default 'dump' directory")
			usedDefaultTarget = true
		}
		target, err = newActualPath(restore.TargetDirectory)
		if err != nil {
			if usedDefaultTarget {
				log.Logv(log.Always, "see mongorestore --help for usage information")
			}
			return fmt.Errorf("mongorestore target '%v' invalid: %v", restore.TargetDirectory, err)
		}
		// handle cases where the user passes in a file instead of a directory
		if !target.IsDir() {
			log.Logv(log.DebugLow, "mongorestore target is a file, not a directory")
			err = restore.handleBSONInsteadOfDirectory(restore.TargetDirectory)
			if err != nil {
				return err
			}
		} else {
			log.Logv(log.DebugLow, "mongorestore target is a directory, not a file")
		}
	}
	if restore.NSOptions.Collection != "" &&
		restore.OutputOptions.NumParallelCollections > 1 &&
		restore.OutputOptions.NumInsertionWorkers == 1 {
		// handle special parallelization case when we are only restoring one collection
		// by mapping -j to insertion workers rather than parallel collections
		log.Logvf(log.DebugHigh,
			"setting number of insertions workers to number of parallel collections (%v)",
			restore.OutputOptions.NumParallelCollections)
		restore.OutputOptions.NumInsertionWorkers = restore.OutputOptions.NumParallelCollections
	}
	if restore.InputOptions.Archive != "" {
		if int(restore.archive.Prelude.Header.ConcurrentCollections) > restore.OutputOptions.NumParallelCollections {
			restore.OutputOptions.NumParallelCollections = int(restore.archive.Prelude.Header.ConcurrentCollections)
			restore.OutputOptions.NumInsertionWorkers = int(restore.archive.Prelude.Header.ConcurrentCollections)
			log.Logvf(log.Always,
				"setting number of parallel collections to number of parallel collections in archive (%v)",
				restore.archive.Prelude.Header.ConcurrentCollections,
			)
		}
	}

	// Create the demux before intent creation, because muted archive intents need
	// to register themselves with the demux directly
	if restore.InputOptions.Archive != "" {
		restore.archive.Demux = archive.CreateDemux(restore.archive.Prelude.NamespaceMetadatas, restore.archive.In)
	}

	switch {
	case restore.InputOptions.Archive != "":
		log.Logvf(log.Always, "preparing collections to restore from")
		err = restore.CreateAllIntents(target)
	case restore.NSOptions.DB != "" && restore.NSOptions.Collection == "":
		log.Logvf(log.Always,
			"building a list of collections to restore from %v dir",
			target.Path())
		err = restore.CreateIntentsForDB(
			restore.NSOptions.DB,
			target,
		)
	case restore.NSOptions.DB != "" && restore.NSOptions.Collection != "" && restore.TargetDirectory == "-":
		log.Logvf(log.Always, "setting up a collection to be read from standard input")
		err = restore.CreateStdinIntentForCollection(
			restore.NSOptions.DB,
			restore.NSOptions.Collection,
		)
	case restore.NSOptions.DB != "" && restore.NSOptions.Collection != "":
		log.Logvf(log.Always, "checking for collection data in %v", target.Path())
		err = restore.CreateIntentForCollection(
			restore.NSOptions.DB,
			restore.NSOptions.Collection,
			target,
		)
	default:
		log.Logvf(log.Always, "preparing collections to restore from")
		err = restore.CreateAllIntents(target)
	}
	if err != nil {
		return fmt.Errorf("error scanning filesystem: %v", err)
	}

	if restore.isMongos && restore.manager.HasConfigDBIntent() && restore.NSOptions.DB == "" {
		return fmt.Errorf("cannot do a full restore on a sharded system - " +
			"remove the 'config' directory from the dump directory first")
	}

	if restore.InputOptions.OplogFile != "" {
		err = restore.CreateIntentForOplog()
		if err != nil {
			return fmt.Errorf("error reading oplog file: %v", err)
		}
	}
	if restore.InputOptions.OplogReplay && restore.manager.Oplog() == nil {
		return fmt.Errorf("no oplog file to replay; make sure you run mongodump with --oplog")
	}
	if restore.manager.GetOplogConflict() {
		return fmt.Errorf("cannot provide both an oplog.bson file and an oplog file with --oplogFile, " +
			"nor can you provide both a local/oplog.rs.bson and a local/oplog.$main.bson file.")
	}

	conflicts := restore.manager.GetDestinationConflicts()
	if len(conflicts) > 0 {
		for _, conflict := range conflicts {
			log.Logvf(log.Always, "%s", conflict.Error())
		}
		return fmt.Errorf("cannot restore with conflicting namespace destinations")
	}

	if restore.OutputOptions.DryRun {
		log.Logvf(log.Always, "dry run completed")
		return nil
	}

	demuxFinished := make(chan interface{})
	var demuxErr error
	if restore.InputOptions.Archive != "" {
		namespaceChan := make(chan string, 1)
		namespaceErrorChan := make(chan error)
		restore.archive.Demux.NamespaceChan = namespaceChan
		restore.archive.Demux.NamespaceErrorChan = namespaceErrorChan

		go func() {
			demuxErr = restore.archive.Demux.Run()
			close(demuxFinished)
		}()
		// consume the new namespace announcement from the demux for all of the special collections
		// that get cached when being read out of the archive.
		// The first regular collection found gets pushed back on to the namespaceChan
		// consume the new namespace announcement from the demux for all of the collections that get cached
		for {
			ns, ok := <-namespaceChan
			// the archive can have only special collections. In that case we keep reading until
			// the namespaces are exhausted, indicated by the namespaceChan being closed.
			if !ok {
				break
			}
			intent := restore.manager.IntentForNamespace(ns)
			if intent == nil {
				return fmt.Errorf("no intent for collection in archive: %v", ns)
			}
			if intent.IsSystemIndexes() ||
				intent.IsUsers() ||
				intent.IsRoles() ||
				intent.IsAuthVersion() {
				log.Logvf(log.DebugLow, "special collection %v found", ns)
				namespaceErrorChan <- nil
			} else {
				// Put the ns back on the announcement chan so that the
				// demultiplexer can start correctly
				log.Logvf(log.DebugLow, "first non special collection %v found."+
					" The demultiplexer will handle it and the remainder", ns)
				namespaceChan <- ns
				break
			}
		}
	}

	// If restoring users and roles, make sure we validate auth versions
	if restore.ShouldRestoreUsersAndRoles() {
		log.Logv(log.Info, "comparing auth version of the dump directory and target server")
		restore.authVersions.Dump, err = restore.GetDumpAuthVersion()
		if err != nil {
			return fmt.Errorf("error getting auth version from dump: %v", err)
		}
		restore.authVersions.Server, err = auth.GetAuthVersion(restore.SessionProvider)
		if err != nil {
			return fmt.Errorf("error getting auth version of server: %v", err)
		}
		err = restore.ValidateAuthVersions()
		if err != nil {
			return fmt.Errorf(
				"the users and roles collections in the dump have an incompatible auth version with target server: %v",
				err)
		}
	}

	err = restore.LoadIndexesFromBSON()
	if err != nil {
		return fmt.Errorf("restore error: %v", err)
	}

	// Restore the regular collections
	if restore.InputOptions.Archive != "" {
		restore.manager.UsePrioritizer(restore.archive.Demux.NewPrioritizer(restore.manager))
	} else if restore.OutputOptions.NumParallelCollections > 1 {
		restore.manager.Finalize(intents.MultiDatabaseLTF)
	} else {
		// use legacy restoration order if we are single-threaded
		restore.manager.Finalize(intents.Legacy)
	}

	restore.termChan = make(chan struct{})

	if err := restore.RestoreIntents(); err != nil {
		return err
	}

	// Restore users/roles
	if restore.ShouldRestoreUsersAndRoles() {
		err = restore.RestoreUsersOrRoles(restore.manager.Users(), restore.manager.Roles())
		if err != nil {
			return fmt.Errorf("restore error: %v", err)
		}
	}

	// Restore oplog
	if restore.InputOptions.OplogReplay {
		err = restore.RestoreOplog()
		if err != nil {
			return fmt.Errorf("restore error: %v", err)
		}
	}

	defer log.Logv(log.Always, "done")

	if restore.InputOptions.Archive != "" {
		<-demuxFinished
		return demuxErr
	}

	return nil
}

func (restore *MongoRestore) getArchiveReader() (rc io.ReadCloser, err error) {
	if restore.InputOptions.Archive == "-" {
		rc = ioutil.NopCloser(restore.InputReader)
	} else {
		targetStat, err := os.Stat(restore.InputOptions.Archive)
		if err != nil {
			return nil, err
		}
		if targetStat.IsDir() {
			defaultArchiveFilePath := filepath.Join(restore.InputOptions.Archive, "archive")
			if restore.InputOptions.Gzip {
				defaultArchiveFilePath = defaultArchiveFilePath + ".gz"
			}
			rc, err = os.Open(defaultArchiveFilePath)
			if err != nil {
				return nil, err
			}
		} else {
			rc, err = os.Open(restore.InputOptions.Archive)
			if err != nil {
				return nil, err
			}
		}
	}
	if restore.InputOptions.Gzip {
		gzrc, err := gzip.NewReader(rc)
		if err != nil {
			return nil, err
		}
		return &util.WrappedReadCloser{gzrc, rc}, nil
	}
	return rc, nil
}

func (restore *MongoRestore) HandleInterrupt() {
	if restore.termChan != nil {
		close(restore.termChan)
	}
}
