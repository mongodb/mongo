// Copyright (C) MerizoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package merizoreplay

import (
	"fmt"
	"github.com/merizodb/merizo-tools/common/options"
	"runtime"
)

// Print the tool version to stdout.  Returns whether or not the version flag
// is specified.
func (o *VersionOptions) PrintVersion() bool {
	if o.Version {
		printVersionInfo()
	}
	return o.Version
}

func printVersionInfo() {
	fmt.Printf("%v version: %v\n", "merizoreplay", options.VersionStr)
	fmt.Printf("git version: %v\n", options.Gitspec)
	fmt.Printf("Go version: %v\n", runtime.Version())
	fmt.Printf("   os: %v\n", runtime.GOOS)
	fmt.Printf("   arch: %v\n", runtime.GOARCH)
	fmt.Printf("   compiler: %v\n", runtime.Compiler)
}
