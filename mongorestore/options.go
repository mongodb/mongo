package mongorestore

type InputOptions struct {
	Objcheck               bool   `long:"objcheck" description:"Validate object before inserting"`
	OplogReplay            bool   `long:"oplogReplay" description:"Replay oplog for point-in-time restore"`
	OplogLimit             string `long:"oplogLimit" description:"Include oplog entries before the provided Timestamp (seconds[:ordinal])"`
	RestoreDBUsersAndRoles bool   `long:"restoreDbUsersAndRoles" description:"Restore user and role definitions for the given database"`
	Directory              string `long:"dir" description:"alternative flag for entering the dump directory"`
}

func (self *InputOptions) Name() string {
	return "input"
}

type OutputOptions struct {
	Drop                   bool   `long:"drop" description:"Drop each collection before import"`
	WriteConcern           string `long:"writeConcern" default:"majority" description:"Write concern options e.g. --writeConcern majority, --writeConcern '{w: 3, wtimeout: 500, fsync: true, j: true}'"`
	NoIndexRestore         bool   `long:"noIndexRestore" description:"Don't restore indexes"`
	NoOptionsRestore       bool   `long:"noOptionsRestore" description:"Don't restore options"`
	KeepIndexVersion       bool   `long:"keepIndexVersion" description:"Don't update index version"`
	MaintainInsertionOrder bool   `long:"maintainInsertionOrder" description:"Preserve order of documents during restoration"`
	NumParallelCollections int    `long:"numParallelCollections" short:"j" description:"Number of collections to restore in parallel" default:"4"`
	StopOnError            bool   `long:"stopOnError" description:"Stop restoring if an error is encountered on insert (off by default)" default:"false"`
}

func (self *OutputOptions) Name() string {
	return "restore"
}
