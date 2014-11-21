// Package options implements mongotop-specific command-line options.
package options

import ()

// Output options for mongotop
type Output struct {
	Locks    bool `long:"locks" description:"Report on use of per-database locks"`
	RowCount int  `long:"rowcount" short:"n" description:"number of stats lines to print (0 for indefinite)"`
	Json     bool `long:"json" description:"Format output as json"`
}

func (self *Output) Name() string {
	return "output"
}

func (self *Output) PostParse() error {
	return nil
}

func (self *Output) Validate() error {
	return nil
}
