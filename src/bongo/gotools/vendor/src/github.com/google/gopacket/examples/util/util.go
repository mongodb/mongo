// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// Package util provides shared utilities for all gopacket examples.
package util

import (
	"flag"
	"log"
	"os"
	"runtime/pprof"
)

var cpuprofile = flag.String("cpuprofile", "", "Where to write CPU profile")

// Run starts up stuff at the beginning of a main function, and returns a
// function to defer until the function completes.  It should be used like this:
//
//   func main() {
//     defer util.Run()()
//     ... stuff ...
//   }
func Run() func() {
	flag.Parse()
	if *cpuprofile != "" {
		f, err := os.Create(*cpuprofile)
		if err != nil {
			log.Fatalf("could not open cpu profile file %q", *cpuprofile)
		}
		pprof.StartCPUProfile(f)
		return func() {
			pprof.StopCPUProfile()
			f.Close()
		}
	}
	return func() {}
}
