package mongorestore

type InputOptions struct {
	Objcheck               bool   `long:"objcheck" description:"validate all objects before inserting"`
	OplogReplay            bool   `long:"oplogReplay" description:"replay oplog for point-in-time restore"`
	OplogLimit             string `long:"oplogLimit" description:"only include oplog entries before the provided Timestamp (seconds[:ordinal])"`
	RestoreDBUsersAndRoles bool   `long:"restoreDbUsersAndRoles" description:"restore user and role definitions for the given database"`
	Directory              string `long:"dir" description:"input directory, use '-' for stdin"`
}

func (_ *InputOptions) Name() string {
	return "input"
}

type OutputOptions struct {
	Drop                   bool   `long:"drop" description:"drop each collection before import"`
	WriteConcern           string `long:"writeConcern" default:"majority" default-mask:"-" description:"write concern options e.g. --writeConcern majority, --writeConcern '{w: 3, wtimeout: 500, fsync: true, j: true}' (defaults to 'majority')"`
	NoIndexRestore         bool   `long:"noIndexRestore" description:"don't restore indexes"`
	NoOptionsRestore       bool   `long:"noOptionsRestore" description:"don't restore collection options"`
	KeepIndexVersion       bool   `long:"keepIndexVersion" description:"don't update index version"`
	MaintainInsertionOrder bool   `long:"maintainInsertionOrder" description:"preserve order of documents during restoration"`
	NumParallelCollections int    `long:"numParallelCollections" short:"j" description:"number of collections to restore in parallel (4 by default)" default:"4" default-mask:"-"`
	StopOnError            bool   `long:"stopOnError" description:"stop restoring if an error is encountered on insert (off by default)"`
}

func (_ *OutputOptions) Name() string {
	return "restore"
}
