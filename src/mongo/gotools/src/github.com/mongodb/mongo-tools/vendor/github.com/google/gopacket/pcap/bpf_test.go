// Copyright 2018 The GoPacket Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.
package pcap

import (
	"testing"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

var (
	snaplen = 65535
	packet  = [...]byte{
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // dst mac
		0x0, 0x11, 0x22, 0x33, 0x44, 0x55, // src mac
		0x08, 0x0, // ether type
		0x45, 0x0, 0x0, 0x3c, 0xa6, 0xc3, 0x40, 0x0, 0x40, 0x06, 0x3d, 0xd8, // ip header
		0xc0, 0xa8, 0x50, 0x2f, // src ip
		0xc0, 0xa8, 0x50, 0x2c, // dst ip
		0xaf, 0x14, // src port
		0x0, 0x50, // dst port
	}
	matchingBPFFilter    = "ip and tcp and port 80"
	nonmatchingBPFFilter = "udp and port 80"
)

func BenchmarkPcapNonmatchingBPFFilter(b *testing.B) {
	bpf, err := NewBPF(layers.LinkTypeEthernet, snaplen, nonmatchingBPFFilter)
	if err != nil {
		b.Fatal("incorrect filter")
	}

	ci := gopacket.CaptureInfo{
		InterfaceIndex: 0,
		CaptureLength:  len(packet),
		Length:         len(packet),
		Timestamp:      time.Now(),
	}

	for i := 0; i < b.N; i++ {
		if bpf.Matches(ci, packet[:]) {
			b.Fatal("filter must not match the packet")
		}
	}
}

func BenchmarkPcapMatchingBPFFilter(b *testing.B) {
	bpf, err := NewBPF(layers.LinkTypeEthernet, snaplen, matchingBPFFilter)
	if err != nil {
		b.Fatal("incorrect filter")
	}

	ci := gopacket.CaptureInfo{
		InterfaceIndex: 0,
		CaptureLength:  len(packet),
		Length:         len(packet),
		Timestamp:      time.Now(),
	}

	for i := 0; i < b.N; i++ {
		if !bpf.Matches(ci, packet[:]) {
			b.Fatal("filter must match the packet")
		}
	}
}
