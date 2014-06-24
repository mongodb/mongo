package options

type OutputFormatOptions struct {
	Fields     string `long:"fields" short:"f" description:"comma separated list of field names\ne.g. -f name,age"`
	FieldFile  string `long:"fieldFile" description:"file with field names - 1 per line"`
	CSV        bool   `long:"csv" description:"export to csv instead of json"`
	OutputFile string `long:"out" description:"output file- if not specified, stdout is used"`
	JSONArray  bool   `long:"jsonArray" description:"output to a json array rather than one object per line"`
}

func (self *OutputFormatOptions) Name() string {
	return "output format"
}

type InputOptions struct {
	Query string `long:"query" short:"q" description:"query filter, as a JSON string, e.g., '{x:{$gt:1}}'"`
	//SlaveOk
	//ForceTableScan
}
