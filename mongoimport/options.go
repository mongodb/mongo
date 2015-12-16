package mongoimport

var Usage = `<options> <file>

Import CSV, TSV or JSON data into MongoDB. If no file is provided, mongoimport reads from stdin.

See http://docs.mongodb.org/manual/reference/program/mongoimport/ for more information.`

// InputOptions defines the set of options for reading input data.
type InputOptions struct {
	// Fields is an option to directly specify comma-separated fields to import to CSV.
	Fields *string `long:"fields" value-name:"<field>[,<field>]*" short:"f" description:"comma separated list of field names, e.g. -f name,age"`

	// FieldFile is a filename that refers to a list of fields to import, 1 per line.
	FieldFile *string `long:"fieldFile" value-name:"<filename>" description:"file with field names - 1 per line"`

	// Specifies the location and name of a file containing the data to import.
	File string `long:"file" value-name:"<filename>" description:"file to import from; if not specified, stdin is used"`

	// Treats the input source's first line as field list (csv and tsv only).
	HeaderLine bool `long:"headerline" description:"use first line in input source as the field list (CSV and TSV only)"`

	// Indicates that the underlying input source contains a single JSON array with the documents to import.
	JSONArray bool `long:"jsonArray" description:"treat input source as a JSON array"`

	// Specifies the file type to import. The default format is JSON, but it’s possible to import CSV and TSV files.
	Type string `long:"type" value-name:"<type>" default:"json" default-mask:"-" description:"input format to import: json, csv, or tsv (defaults to 'json')"`
}

// Name returns a description of the InputOptions struct.
func (_ *InputOptions) Name() string {
	return "input"
}

// IngestOptions defines the set of options for storing data.
type IngestOptions struct {
	// Drops target collection before importing.
	Drop bool `long:"drop" description:"drop collection before inserting documents"`

	// Ignores fields with empty values in CSV and TSV imports.
	IgnoreBlanks bool `long:"ignoreBlanks" description:"ignore fields with empty values in CSV and TSV"`

	// Indicates that documents will be inserted in the order of their appearance in the input source.
	MaintainInsertionOrder bool `long:"maintainInsertionOrder" description:"insert documents in the order of their appearance in the input source"`

	// Sets the number of insertion routines to use
	NumInsertionWorkers int `short:"j" value-name:"<number>" long:"numInsertionWorkers" description:"number of insert operations to run concurrently (defaults to 1)" default:"1" default-mask:"-"`

	// Forces mongoimport to halt the import operation at the first insert or upsert error.
	StopOnError bool `long:"stopOnError" description:"stop importing at first insert/upsert error"`

	// Modifies the import process to update existing objects in the database if they match --upsertFields.
	Upsert bool `long:"upsert" description:"insert or update objects that already exist"`

	// Specifies a list of fields for the query portion of the upsert; defaults to _id field.
	UpsertFields string `long:"upsertFields" value-name:"<field>[,<field>]*" description:"comma-separated fields for the query part of the upsert"`

	// Sets write concern level for write operations.
	WriteConcern string `long:"writeConcern" default:"majority" value-name:"<write-concern-specifier>" default-mask:"-" description:"write concern options e.g. --writeConcern majority, --writeConcern '{w: 3, wtimeout: 500, fsync: true, j: true}' (defaults to 'majority')"`

	// Indicates that the server should bypass document validation on import.
	BypassDocumentValidation bool `long:"bypassDocumentValidation" description:"bypass document validation"`
}

// Name returns a description of the IngestOptions struct.
func (_ *IngestOptions) Name() string {
	return "ingest"
}
