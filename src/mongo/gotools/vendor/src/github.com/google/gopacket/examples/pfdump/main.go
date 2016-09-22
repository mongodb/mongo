// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// The pfdump binary implements a tcpdump-like command line tool with gopacket
// using pfring as a backend data collection mechanism.
package main

import (
	"flag"
	"fmt"
	"github.com/google/gopacket/dumpcommand"
	"github.com/google/gopacket/examples/util"
	"github.com/google/gopacket/pfring"
	"log"
	"os"
	"strings"
)

var iface = flag.String("i", "eth0", "Interface to read packets from")
var snaplen = flag.Int("s", 65536, "Snap length (number of bytes max to read per packet")
var cluster = flag.Int("cluster", -1, "If >= 0, sets the pfring cluster to this value")
var clustertype = flag.Int("clustertype", int(pfring.ClusterPerFlow), "Cluster type")

func main() {
	defer util.Run()()
	var ring *pfring.Ring
	var err error
	if ring, err = pfring.NewRing(*iface, uint32(*snaplen), pfring.FlagPromisc); err != nil {
		log.Fatalln("pfring ring creation error:", err)
	}
	if len(flag.Args()) > 0 {
		bpffilter := strings.Join(flag.Args(), " ")
		fmt.Fprintf(os.Stderr, "Using BPF filter %q\n", bpffilter)
		if err = ring.SetBPFFilter(bpffilter); err != nil {
			log.Fatalln("BPF filter error:", err)
		}
	}
	if *cluster >= 0 {
		if err = ring.SetCluster(*cluster, pfring.ClusterType(*clustertype)); err != nil {
			log.Fatalln("pfring SetCluster error:", err)
		}
	}
	if err = ring.SetSocketMode(pfring.ReadOnly); err != nil {
		log.Fatalln("pfring SetSocketMode error:", err)
	} else if err = ring.Enable(); err != nil {
		log.Fatalln("pfring Enable error:", err)
	}
	dumpcommand.Run(ring)
}
