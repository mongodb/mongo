package mongoimport

type InputOptions struct {
	// Fields is an option to directly specify comma-separated fields to import to CSV.
	Fields *string `long:"fields" short:"f" description:"comma separated list of field names e.g. -f name,age"`

	// FieldFile is a filename that refers to a list of fields to import, 1 per line.
	FieldFile *string `long:"fieldFile" description:"file with field names - 1 per line"`

	// Specifies the location and name of a file containing the data to import.
	File string `long:"file" description:"file to import from; if not specified stdin is used"`

	// Treats the input source's first line as field list (csv and tsv only)
	HeaderLine bool `long:"headerline" description:"use first line in input source as the field list (csv and tsv only)"`

	// Indicates that the underlying input source contains a single JSON array with the documents to import.
	JSONArray bool `long:"jsonArray" description:"treat input source as a JSON array"`

	// Specifies the file type to import. The default format is JSON, but it’s possible to import CSV and TSV files.
	Type string `long:"type" default:"json" description:"type of file to import (json, csv, tsv)"`
}

func (self *InputOptions) Name() string {
	return "mongoimport input"
}

type IngestOptions struct {
	// Drops target collection before importing.
	Drop bool `long:"drop" description:"drop collection first"`

	// Ignores fields with empty values in CSV and TSV imports.
	IgnoreBlanks bool `long:"ignoreBlanks" description:"ignore fields with empty value in CSV and TSV"`

	// Indicates that documents will be inserted in the order of their appearance in the input source.
	MaintainInsertionOrder bool `long:"maintainInsertionOrder" description:"insert documents in the order of their appearance in the input source"`

	// Specifies a list of fields for the query portion of the upsert.
	UpsertFields string `long:"upsertFields" description:"comma-separated fields for the query part of the upsert"`

	// Forces mongoimport to halt the import operation at the first insert or upsert error.
	StopOnError bool `long:"stopOnError" description:"stop importing at first insert/upsert error"`

	// Sets write concern level for write operations.
	WriteConcern string `long:"writeConcern" default:"majority" description:"write concern options e.g. --writeConcern majority, --writeConcern '{w: 3, wtimeout: 500, fsync: true, j: true}'"`
}

func (self *IngestOptions) Name() string {
	return "mongoimport ingest"
}
