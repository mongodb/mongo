package mongorestore

//Usage describes basic usage of mongorestore
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
	Drop                     bool   `long:"drop" description:"drop each collection before import"`
	WriteConcern             string `long:"writeConcern" value-name:"<write-concern>" default:"majority" default-mask:"-" description:"write concern options e.g. --writeConcern majority, --writeConcern '{w: 3, wtimeout: 500, fsync: true, j: true}' (defaults to 'majority')"`
	NoIndexRestore           bool   `long:"noIndexRestore" description:"don't restore indexes"`
	NoOptionsRestore         bool   `long:"noOptionsRestore" description:"don't restore collection options"`
	KeepIndexVersion         bool   `long:"keepIndexVersion" description:"don't update index version"`
	MaintainInsertionOrder   bool   `long:"maintainInsertionOrder" description:"preserve order of documents during restoration"`
	NumParallelCollections   int    `long:"numParallelCollections" short:"j" description:"number of collections to restore in parallel (4 by default)" default:"4" default-mask:"-"`
	NumInsertionWorkers      int    `long:"numInsertionWorkersPerCollection" description:"number of insert operations to run concurrently per collection (1 by default)" default:"1" default-mask:"-"`
	StopOnError              bool   `long:"stopOnError" description:"stop restoring if an error is encountered on insert (off by default)"`
	BypassDocumentValidation bool   `long:"bypassDocumentValidation" description:"bypass document validation"`
}

// Name returns a human-readable group name for output options.
func (*OutputOptions) Name() string {
	return "restore"
}
