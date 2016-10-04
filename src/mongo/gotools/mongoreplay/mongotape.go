package mongoreplay

// Options stores settings for any mongoreplay command
type Options struct {
	Verbosity []bool `short:"v" long:"verbosity" description:"increase the detail regarding the tools performance on the input file that is output to logs (include multiple times for increased logging verbosity, e.g. -vvv)"`
	Debug     []bool `short:"d" long:"debug" description:"increase the detail regarding the operations and errors of the tool that is output to the logs(include multiple times for increased debugging information, e.g. -ddd)"`
	Silent    bool   `short:"s" long:"silent" description:"silence all log output"`
	Version   bool   `long:"version" description:"display the version and exit"`
}

// SetLogging sets the verbosity/debug level for log output.
func (opts *Options) SetLogging() {
	v := len(opts.Verbosity)
	d := len(opts.Debug)
	if opts.Silent {
		v = -1
		d = -1
	}
	userInfoLogger.setVerbosity(v)
	toolDebugLogger.setVerbosity(d)
}
