package mongostat

var Usage = `<options> <polling interval in seconds>

Monitor basic MongoDB server statistics.

See http://docs.mongodb.org/manual/reference/program/mongostat/ for more information.`

// StatOptions defines the set of options to use for configuring mongostat.
type StatOptions struct {
	NoHeaders bool `long:"noheaders" description:"don't output column names"`
	RowCount  int  `long:"rowcount" value-name:"<count>" short:"n" description:"number of stats lines to print (0 for indefinite)"`
	Discover  bool `long:"discover" description:"discover nodes and display stats for all"`
	Http      bool `long:"http" description:"use HTTP instead of raw db connection"`
	All       bool `long:"all" description:"all optional fields"`
	Json      bool `long:"json" description:"output as JSON rather than a formatted table"`
}

// Name returns a human-readable group name for mongostat options.
func (_ *StatOptions) Name() string {
	return "stat"
}
