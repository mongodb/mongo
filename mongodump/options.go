package mongodump

type InputOptions struct {
	Query     string `long:"query" short:"q" description:"query filter, as a JSON string, e.g., '{x:{$gt:1}}'"`
	TableScan bool   `long:"forceTableScan" description:"force a table scan"`
}

func (self *InputOptions) Name() string {
	return "query"
}

type OutputOptions struct {
	Out                        string   `long:"out" short:"o" description:"Output directory or - for stdout" default:"dump"`
	Repair                     bool     `long:"repair" description:"Try to recover docs from damaged data files (not supported by all storage engines)"`
	Oplog                      bool     `long:"oplog" description:"Use oplog for point-in-time snapshotting"`
	DumpDBUsersAndRoles        bool     `long:"dumpDbUsersAndRoles" description:"Dump user and role definitions for the given database"`
	ExcludedCollections        []string `long:"excludeCollection" description:"Collections to exclude from the dump"`
	ExcludedCollectionPrefixes []string `long:"excludeCollectionsWithPrefix" description:"Exclude all collections from the dump that have the given prefix"`
}

func (self *OutputOptions) Name() string {
	return "output"
}
