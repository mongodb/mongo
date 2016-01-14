package bsondump

var Usage = `<options> <file>

View and debug .bson files.

See http://docs.mongodb.org/manual/reference/program/bsondump/ for more information.`

type BSONDumpOptions struct {
	// Format to display the BSON data file
	Type string `long:"type" value-name:"<type>" default:"json" default-mask:"-" description:"type of output: debug, json (default 'json')"`

	// Validate each BSON document before displaying
	ObjCheck bool `long:"objcheck" description:"validate BSON during processing"`

	// Display JSON data with indents
	Pretty bool `long:"pretty" description:"output JSON formatted to be human-readable"`

	// Path to input BSON file
	BSONFileName string `long:"bsonFile" description:"path to BSON file to dump to JSON; default is stdin"`

	// Path to output file
	OutFileName string `long:"outFile" description:"path to output file to dump BSON to; default is stdout"`
}

func (_ *BSONDumpOptions) Name() string {
	return "output"
}

func (_ *BSONDumpOptions) PostParse() error {
	return nil
}

func (_ *BSONDumpOptions) Validate() error {
	return nil
}
