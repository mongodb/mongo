package options

type InputOptions struct {
	// Fields is an option to directly specify comma-separated fields to import to CSV
	Fields string `long:"fields" short:"f" description:"comma separated list of field names e.g. -f name,age"`

	// FieldFile is a filename that refers to a list of fields to import, 1 per line
	FieldFile string `long:"fieldFile" description:"file with field names - 1 per line"`

	// JSONArray if set will import the documents an array of JSON doccuments
	JSONArray bool `long:"jsonArray" description:"output to a JSON array rather than one object per line"`

	// Specifies the file type to import. The default format is JSON, but it’s possible to import CSV and TSV files.
	Type string `long:"type" default:"json" description:"type of file to import (json, csv, tsv only)"`

	// Specifies the location and name of a file containing the data to import.
	// If you do not specify a file, mongoimport reads data from standard input (e.g. “stdin”).
	File string `long:"file" description:"file to import from; if not specified stdin is used"`

	// If using --type csv or --type tsv, uses the first line as field names.
	// Otherwise, mongoimport will import the first line as a distinct document.
	HeaderLine bool `long:"headerline" description:"if given, the first line in input file will be used as the field list (csv and tsv only)"`
}

func (self *InputOptions) Name() string {
	return "mongoimport input"
}

type IngestOptions struct {
	// Modifies the import process so that the target instance drops every
	// collection before importing the collection from the input.
	Drop bool `long:"drop" description:"drop collection first"`

	// Ignores empty fields in CSV and TSV imports. If not specified,
	// mongoimport creates fields without values in imported documents.
	IgnoreBlanks bool `long:"ignoreBlanks" description:"if given, empty fields in CSV and TSV will be ignored"`

	// Modifies the import process to update existing objects in the database if
	// they match an imported object, while inserting all other objects.
	// If you do not specify a field or fields using the --upsertFields
	// mongoimport will upsert on the basis of the _id field.
	Upsert bool `long:"upsert" description:"insert or update objects that already exist"`

	// Specifies a list of fields for the query portion of the upsert.
	// Use this option if the _id fields in the existing documents don’t match
	// the field in the document, but another field or field combination can
	// uniquely identify documents as a basis for performing upsert operations.
	UpsertFields string `long:"upsertFields" description:"comma-separated fields for the query part of the upsert. You should make sure this is indexed"`

	// Forces mongoimport to halt the import operation at the first error
	// rather than continuing the operation despite errors.
	StopOnError bool `long:"stopOnError" description:"stop importing at first error rather than continuing"`

	/*	CPU and Network I/O related configurations	*/

	// Specifies the number of operating system threads to use during the import process
	MaintainInsertionOrder bool `long:"maintainInsertionOrder" description:"if given, documents should be inserted in the order of their appearance in the input source"`

	// Specifies the number of operating system threads to use during the import process
	// TODO: hide this option
	NumOSThreads *int `long:"numOsThreads" description:"number of operating system threads to use (defaults to the number of logical CPUs)"`

	// Specifies the number of threads to use in processing data read from the input source
	NumDecodingWorkers *int `long:"numDecodingWorkers" description:"number of goroutines to use for converting JSON to BSON (defaults to the number of logical CPUs)"`

	// Specifies the number of threads to use in sending processed data over to the server
	NumInsertionWorkers *int `long:"numInsertionWorkers" description:"number of goroutines to use in ingesting data (defaults to 1)"`

	// Specifies the maximum number of documents in each batch sent over to the server
	BatchSize *int `long:"batchSize"  description:"number of documents to insert in a single batch"`

	// Specifies the write concern for each write operation that mongorestore writes to the target database.
	// By default, mongoimport waits for a majority of members from the replica set to respond before returning.
	WriteConcern string `long:"writeConcern" default:"majority" description:"write concern options e.g. --writeConcern majority, --writeConcern '{w: 3, wtimeout: 500, fsync: true, j: true}'"`
}

func (self *IngestOptions) Name() string {
	return "mongoimport ingest"
}
