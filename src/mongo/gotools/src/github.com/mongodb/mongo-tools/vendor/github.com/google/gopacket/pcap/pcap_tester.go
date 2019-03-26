// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// +build ignore

// This binary tests that PCAP packet capture is working correctly by issuing
// HTTP requests, then making sure we actually capture data off the wire.
package main

import (
	"errors"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"time"

	"github.com/google/gopacket/pcap"
)

var mode = flag.String("mode", "basic", "One of: basic,filtered,timestamp")

func generatePackets() {
	if resp, err := http.Get("http://code.google.com"); err != nil {
		log.Printf("Could not get HTTP: %v", err)
	} else {
		resp.Body.Close()
	}
}

func main() {
	flag.Parse()
	ifaces, err := pcap.FindAllDevs()
	if err != nil {
		log.Fatal(err)
	}
	for _, iface := range ifaces {
		log.Printf("Trying capture on %q", iface.Name)
		if err := tryCapture(iface); err != nil {
			log.Printf("Error capturing on %q: %v", iface.Name, err)
		} else {
			log.Printf("Successfully captured on %q", iface.Name)
			return
		}
	}
	os.Exit(1)
}

func tryCapture(iface pcap.Interface) error {
	if iface.Name[:2] == "lo" {
		return errors.New("skipping loopback")
	}
	var h *pcap.Handle
	var err error
	switch *mode {
	case "basic":
		h, err = pcap.OpenLive(iface.Name, 65536, false, time.Second*3)
		if err != nil {
			return fmt.Errorf("openlive: %v", err)
		}
		defer h.Close()
	case "filtered":
		h, err = pcap.OpenLive(iface.Name, 65536, false, time.Second*3)
		if err != nil {
			return fmt.Errorf("openlive: %v", err)
		}
		defer h.Close()
		if err := h.SetBPFFilter("port 80 or port 443"); err != nil {
			return fmt.Errorf("setbpf: %v", err)
		}
	case "timestamp":
		u, err := pcap.NewInactiveHandle(iface.Name)
		if err != nil {
			return err
		}
		defer u.CleanUp()
		if err = u.SetSnapLen(65536); err != nil {
			return err
		} else if err = u.SetPromisc(false); err != nil {
			return err
		} else if err = u.SetTimeout(time.Second * 3); err != nil {
			return err
		}
		sources := u.SupportedTimestamps()
		if len(sources) == 0 {
			return errors.New("no supported timestamp sources")
		} else if err := u.SetTimestampSource(sources[0]); err != nil {
			return fmt.Errorf("settimestampsource(%v): %v", sources[0], err)
		} else if h, err = u.Activate(); err != nil {
			return fmt.Errorf("could not activate: %v", err)
		}
		defer h.Close()
	default:
		panic("Invalid --mode: " + *mode)
	}
	go generatePackets()
	h.ReadPacketData() // Do one dummy read to clear any timeouts.
	data, ci, err := h.ReadPacketData()
	if err != nil {
		return fmt.Errorf("readpacketdata: %v", err)
	}
	log.Printf("Read packet, %v bytes, CI: %+v", len(data), ci)
	return nil
}
