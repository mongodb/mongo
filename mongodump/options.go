package mongodump

type InputOptions struct {
	Query     string `long:"query" short:"q" description:"query filter, as a JSON string, e.g., '{x:{$gt:1}}'"`
	TableScan bool   `long:"forceTableScan" description:"force a table scan"`
}

func (self *InputOptions) Name() string {
	return "query"
}

type OutputOptions struct {
	Out                        string   `long:"out" short:"o" description:"output directory, or '-' for stdout (defaults to 'dump')" default:"dump" default-mask:"-"`
	Repair                     bool     `long:"repair" description:"try to recover documents from damaged data files (not supported by all storage engines)"`
	Oplog                      bool     `long:"oplog" description:"use oplog for taking a point-in-time snapshot"`
	DumpDBUsersAndRoles        bool     `long:"dumpDbUsersAndRoles" description:"dump user and role definitions for the specified database"`
	ExcludedCollections        []string `long:"excludeCollection" description:"collections to exclude from the dump"`
	ExcludedCollectionPrefixes []string `long:"excludeCollectionsWithPrefix" description:"exclude all collections from the dump that have the given prefix"`
}

func (self *OutputOptions) Name() string {
	return "output"
}
