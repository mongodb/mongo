// Package options implements mongostat-specific command-line options.
package options

import ()

// Output options for mongostat
type StatOptions struct {
	NoHeaders bool `long:"noheaders" description:"don't output column names"`
	RowCount  int  `long:"rowcount" short:"n" description:"number of stats lines to print (0 for indefinite)"`
	Discover  bool `long:"discover" description:"discover nodes and display stats for all"`
	Http      bool `long:"http" description:"use http instead of raw db connection"`
	All       bool `long:"all" description:"all optional fields"`
}

func (statOpts *StatOptions) Name() string {
	return "stat"
}
