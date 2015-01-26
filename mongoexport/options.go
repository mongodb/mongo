package mongoexport

var Usage = `<options>

Export data from MongoDB in CSV or JSON format.

See http://docs.mongodb.org/manual/reference/program/mongoexport/ for more information.`

type OutputFormatOptions struct {
	//Fields is an option to directly specify comma-separated fields to export to CSV
	Fields string `long:"fields" short:"f" description:"comma separated list of field names, e.g. -f name,age"`

	//FieldFile is a filename that refers to a list of fields to export, 1 per line
	FieldFile string `long:"fieldFile" description:"file with field names - 1 per line"`

	//Type selects the type of output to export as (json or csv)
	Type string `long:"type" default:"json" default-mask:"-" description:"the output format, either json or csv (defaults to 'json')"`

	//OutputFile specifies an output file path.
	OutputFile string `long:"out" short:"o" description:"output file; if not specified, stdout is used"`

	//JSONArray if set will export the documents an array of json docs
	JSONArray bool `long:"jsonArray" description:"output to a JSON array rather than one object per line"`

	//Pretty to format the JSON results
	Pretty bool `long:"pretty" description:"output JSON formatted to be human-readable"`
}

func (_ *OutputFormatOptions) Name() string {
	return "output"
}

type InputOptions struct {
	Query          string `long:"query" short:"q" description:"query filter, as a JSON string, e.g., '{x:{$gt:1}}'"`
	SlaveOk        bool   `long:"slaveOk" short:"k" description:"allow secondary reads if available (default true)" default:"true" default-mask:"-"`
	ForceTableScan bool   `long:"forceTableScan" description:"force a table scan (do not use $snapshot)"`
	Skip           int    `long:"skip" description:"number of documents to skip"`
	Limit          int    `long:"limit" description:"limit the number of documents to export"`
	Sort           string `long:"sort" description:"sort order, as a JSON string, e.g. '{x:1}'"`
}

func (_ *InputOptions) Name() string {
	return "querying"
}
