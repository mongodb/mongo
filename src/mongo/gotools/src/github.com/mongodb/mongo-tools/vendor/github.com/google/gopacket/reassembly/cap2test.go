// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// +build ignore

package main

import (
	"bytes"
	"flag"
	"fmt"
	"log"
	"os"
	"strings"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
	"github.com/google/gopacket/pcap"
)

var input = flag.String("i", "", "Input filename")

func main() {
	var handler *pcap.Handle
	var err error
	flag.Parse()
	if *input == "" {
		log.Fatalf("Please specify input filename")
	}
	if handler, err = pcap.OpenOffline(*input); err != nil {
		log.Fatalf("Failed to open: %s: %s", *input, err)
	}
	args := flag.Args()
	if len(args) > 0 {
		filter := strings.Join(args, " ")
		if err := handler.SetBPFFilter(filter); err != nil {
			log.Fatalf("Failed to set BPF filter \"%s\": %s", filter, err)
		}
		handler.Stats()
	}
	var decoder gopacket.Decoder
	var ok bool
	linkType := fmt.Sprintf("%s", handler.LinkType())
	if decoder, ok = gopacket.DecodersByLayerName[linkType]; !ok {
		log.Fatalf("Failed to find decoder to pcap's linktype %s", linkType)
	}
	source := gopacket.NewPacketSource(handler, decoder)
	count := uint64(0)
	pktNonTcp := uint64(0)
	pktTcp := uint64(0)
	fmt.Println("test([]testSequence{")
	for packet := range source.Packets() {
		count++
		tcp := packet.Layer(layers.LayerTypeTCP)
		if tcp == nil {
			pktNonTcp++
			continue
		} else {
			pktTcp++
			tcp := tcp.(*layers.TCP)
			//fmt.Printf("packet: %s\n", tcp)
			var b bytes.Buffer
			b.WriteString("{\n")
			// TCP
			b.WriteString("tcp: layers.TCP{\n")
			if tcp.SYN {
				b.WriteString("  SYN: true,\n")
			}
			if tcp.ACK {
				b.WriteString("  ACK: true,\n")
			}
			if tcp.RST {
				b.WriteString("  RST: true,\n")
			}
			if tcp.FIN {
				b.WriteString("  FIN: true,\n")
			}
			b.WriteString(fmt.Sprintf("  SrcPort: %d,\n", tcp.SrcPort))
			b.WriteString(fmt.Sprintf("  DstPort: %d,\n", tcp.DstPort))
			b.WriteString(fmt.Sprintf("  Seq: %d,\n", tcp.Seq))
			b.WriteString(fmt.Sprintf("  Ack: %d,\n", tcp.Ack))
			b.WriteString("  BaseLayer: layers.BaseLayer{Payload: []byte{")
			for _, p := range tcp.Payload {
				b.WriteString(fmt.Sprintf("%d,", p))
			}
			b.WriteString("}},\n")
			b.WriteString("},\n")
			// CaptureInfo
			b.WriteString("ci: gopacket.CaptureInfo{\n")
			ts := packet.Metadata().CaptureInfo.Timestamp
			b.WriteString(fmt.Sprintf("  Timestamp: time.Unix(%d,%d),\n", ts.Unix(), ts.Nanosecond()))
			b.WriteString("},\n")
			// Struct
			b.WriteString("},\n")
			fmt.Print(b.String())
		}

	}
	fmt.Println("})")

	fmt.Fprintf(os.Stderr, "Total: %d, TCP: %d, non-TCP: %d\n", count, pktTcp, pktNonTcp)
}
