// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"fmt"
	"runtime"
)

// Print the tool version to stdout.  Returns whether or not the version flag
// is specified.
func (o *VersionOptions) PrintVersion(versionStr, gitCommit string) bool {
	if o.Version {
		printVersionInfo(versionStr, gitCommit)
	}
	return o.Version
}

func printVersionInfo(versionStr, gitCommit string) {
	fmt.Printf("%v version: %v\n", "mongoreplay", versionStr)
	fmt.Printf("git version: %v\n", gitCommit)
	fmt.Printf("Go version: %v\n", runtime.Version())
	fmt.Printf("   os: %v\n", runtime.GOOS)
	fmt.Printf("   arch: %v\n", runtime.GOARCH)
	fmt.Printf("   compiler: %v\n", runtime.Compiler)
}
