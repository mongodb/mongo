// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

// Options stores settings for any mongoreplay command
type Options struct {
	Verbosity []bool `short:"v" long:"verbosity" description:"increase the detail regarding the tools performance on the input file that is output to logs (include multiple times for increased logging verbosity, e.g. -vvv)"`
	Debug     []bool `short:"d" long:"debug" description:"increase the detail regarding the operations and errors of the tool that is output to the logs(include multiple times for increased debugging information, e.g. -ddd)"`
	Silent    bool   `short:"s" long:"silent" description:"silence all log output"`
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
	if d > 0 || v > 0 {
		printVersionInfo()
	}
}

type VersionOptions struct {
	Version bool `long:"version" description:"display the version and exit"`
}
