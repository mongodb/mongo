// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// The pcaplay binary load an offline capture (pcap file) and replay
// it on the select interface, with an emphasis on packet timing
package main

import (
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"strings"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/examples/util"
	"github.com/google/gopacket/pcap"
)

var iface = flag.String("i", "eth0", "Interface to write packets to")
var fname = flag.String("r", "", "Filename to read from")
var fast = flag.Bool("f", false, "Send each packets as fast as possible")

var lastTS time.Time
var lastSend time.Time

var start time.Time
var bytesSent int

func writePacketDelayed(handle *pcap.Handle, buf []byte, ci gopacket.CaptureInfo) {
	if ci.CaptureLength != ci.Length {
		// do not write truncated packets
		return
	}

	intervalInCapture := ci.Timestamp.Sub(lastTS)
	elapsedTime := time.Since(lastSend)

	if (intervalInCapture > elapsedTime) && !lastSend.IsZero() {
		time.Sleep(intervalInCapture - elapsedTime)
	}

	lastSend = time.Now()
	writePacket(handle, buf)
	lastTS = ci.Timestamp
}

func writePacket(handle *pcap.Handle, buf []byte) error {
	if err := handle.WritePacketData(buf); err != nil {
		log.Printf("Failed to send packet: %s\n", err)
		return err
	}
	return nil
}

func pcapInfo(filename string) (start time.Time, end time.Time, packets int, size int) {
	handleRead, err := pcap.OpenOffline(*fname)
	if err != nil {
		log.Fatal("PCAP OpenOffline error (handle to read packet):", err)
	}

	var previousTs time.Time
	var deltaTotal time.Duration

	for {
		data, ci, err := handleRead.ReadPacketData()
		if err != nil && err != io.EOF {
			log.Fatal(err)
		} else if err == io.EOF {
			break
		} else {

			if start.IsZero() {
				start = ci.Timestamp
			}
			end = ci.Timestamp
			packets++
			size += len(data)

			if previousTs.IsZero() {
				previousTs = ci.Timestamp
			} else {
				deltaTotal += ci.Timestamp.Sub(previousTs)
				previousTs = ci.Timestamp
			}
		}
	}
	sec := int(deltaTotal.Seconds())
	if sec == 0 {
		sec = 1
	}
	fmt.Printf("Avg packet rate %d/s\n", packets/sec)
	return start, end, packets, size
}

func main() {
	defer util.Run()()

	// Sanity checks
	if *fname == "" {
		log.Fatal("Need a input file")
	}

	// Open PCAP file + handle potential BPF Filter
	handleRead, err := pcap.OpenOffline(*fname)
	if err != nil {
		log.Fatal("PCAP OpenOffline error (handle to read packet):", err)
	}
	defer handleRead.Close()
	if len(flag.Args()) > 0 {
		bpffilter := strings.Join(flag.Args(), " ")
		fmt.Fprintf(os.Stderr, "Using BPF filter %q\n", bpffilter)
		if err = handleRead.SetBPFFilter(bpffilter); err != nil {
			log.Fatal("BPF filter error:", err)
		}
	}
	// Open up a second pcap handle for packet writes.
	handleWrite, err := pcap.OpenLive(*iface, 65536, true, pcap.BlockForever)
	if err != nil {
		log.Fatal("PCAP OpenLive error (handle to write packet):", err)
	}
	defer handleWrite.Close()

	start = time.Now()
	pkt := 0
	tsStart, tsEnd, packets, size := pcapInfo(*fname)

	// Loop over packets and write them
	for {
		data, ci, err := handleRead.ReadPacketData()
		switch {
		case err == io.EOF:
			fmt.Printf("\nFinished in %s", time.Since(start))
			return
		case err != nil:
			log.Printf("Failed to read packet %d: %s\n", pkt, err)
		default:
			if *fast {
				writePacket(handleWrite, data)
			} else {
				writePacketDelayed(handleWrite, data, ci)
			}

			bytesSent += len(data)
			duration := time.Since(start)
			pkt++

			if duration > time.Second {
				rate := bytesSent / int(duration.Seconds())
				remainingTime := tsEnd.Sub(tsStart) - duration
				fmt.Printf("\rrate %d kB/sec - sent %d/%d kB - %d/%d packets - remaining time %s",
					rate/1000, bytesSent/1000, size/1000,
					pkt, packets, remainingTime)
			}
		}
	}

}
