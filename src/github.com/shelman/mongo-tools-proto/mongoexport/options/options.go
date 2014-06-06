package options

type OutputFormatOptions struct {
	FieldFile  string `long:"fieldFile" description:"file with field names - 1 per line"`
	CSV        bool   `long:"csv" description:"export to csv instead of json"`
	OutputFile string `long:"out" description:"output file- if not specified, stdout is used"`
	JSONArray  bool   `long:"jsonArray" description:"output to a json array rather than one object per line"`
}

func (self *OutputFormatOptions) Name() string {
	return "output format"
}
