// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package pcap

import (
	"fmt"
	"io"
	"log"
	"testing"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

func TestPcapFileRead(t *testing.T) {
	for _, file := range []struct {
		filename       string
		num            int
		expectedLayers []gopacket.LayerType
	}{
		{"test_loopback.pcap",
			24,
			[]gopacket.LayerType{
				layers.LayerTypeLoopback,
				layers.LayerTypeIPv6,
				layers.LayerTypeTCP,
			},
		},
		{"test_ethernet.pcap",
			16,
			[]gopacket.LayerType{
				layers.LayerTypeEthernet,
				layers.LayerTypeIPv4,
				layers.LayerTypeTCP,
			},
		},
		{"test_dns.pcap",
			10,
			[]gopacket.LayerType{
				layers.LayerTypeEthernet,
				layers.LayerTypeIPv4,
				layers.LayerTypeUDP,
				layers.LayerTypeDNS,
			},
		},
	} {
		t.Logf("\n\n\n\nProcessing file %s\n\n\n\n", file.filename)

		packets := []gopacket.Packet{}
		if handle, err := OpenOffline(file.filename); err != nil {
			t.Fatal(err)
		} else {
			packetSource := gopacket.NewPacketSource(handle, handle.LinkType())
			for packet := range packetSource.Packets() {
				packets = append(packets, packet)
			}
		}
		if len(packets) != file.num {
			t.Fatal("Incorrect number of packets, want", file.num, "got", len(packets))
		}
		for i, p := range packets {
			t.Log(p.Dump())
			for _, layertype := range file.expectedLayers {
				if p.Layer(layertype) == nil {
					t.Fatal("Packet", i, "has no layer type\n%s", layertype, p.Dump())
				}
			}
		}
	}
}

func TestBPF(t *testing.T) {
	handle, err := OpenOffline("test_ethernet.pcap")
	if err != nil {
		t.Fatal(err)
	}

	for _, expected := range []struct {
		expr   string
		Error  bool
		Result bool
	}{
		{"foobar", true, false},
		{"tcp[tcpflags] & (tcp-syn|tcp-ack) == (tcp-syn|tcp-ack)", false, true},
		{"tcp[tcpflags] & (tcp-syn|tcp-ack) == tcp-ack", false, true},
		{"udp", false, false},
	} {
		data, ci, err := handle.ReadPacketData()
		if err != nil {
			t.Fatal(err)
		}
		t.Log("Testing filter", expected.expr)
		if bpf, err := handle.NewBPF(expected.expr); err != nil {
			if !expected.Error {
				t.Error(err, "while compiling filter was unexpected")
			}
		} else if expected.Error {
			t.Error("expected error but didn't see one")
		} else if matches := bpf.Matches(ci, data); matches != expected.Result {
			t.Error("Filter result was", matches, "but should be", expected.Result)
		}
	}
}

func TestBPFInstruction(t *testing.T) {
	handle, err := OpenOffline("test_ethernet.pcap")
	if err != nil {
		t.Fatal(err)
	}

	cntr := 0
	oversizedBpfInstructionBuffer := [MaxBpfInstructions + 1]BPFInstruction{}

	for _, expected := range []struct {
		BpfInstruction []BPFInstruction
		Error          bool
		Result         bool
	}{
		// {"foobar", true, false},
		{[]BPFInstruction{}, true, false},

		// {"tcp[tcpflags] & (tcp-syn|tcp-ack) == (tcp-syn|tcp-ack)", false, true},
		// tcpdump -dd 'tcp[tcpflags] & (tcp-syn|tcp-ack) == (tcp-syn|tcp-ack)'
		{[]BPFInstruction{
			{0x28, 0, 0, 0x0000000c},
			{0x15, 0, 9, 0x00000800},
			{0x30, 0, 0, 0x00000017},
			{0x15, 0, 7, 0x00000006},
			{0x28, 0, 0, 0x00000014},
			{0x45, 5, 0, 0x00001fff},
			{0xb1, 0, 0, 0x0000000e},
			{0x50, 0, 0, 0x0000001b},
			{0x54, 0, 0, 0x00000012},
			{0x15, 0, 1, 0x00000012},
			{0x6, 0, 0, 0x0000ffff},
			{0x6, 0, 0, 0x00000000},
		}, false, true},

		// {"tcp[tcpflags] & (tcp-syn|tcp-ack) == tcp-ack", false, true},
		// tcpdump -dd 'tcp[tcpflags] & (tcp-syn|tcp-ack) == tcp-ack'
		{[]BPFInstruction{
			{0x28, 0, 0, 0x0000000c},
			{0x15, 0, 9, 0x00000800},
			{0x30, 0, 0, 0x00000017},
			{0x15, 0, 7, 0x00000006},
			{0x28, 0, 0, 0x00000014},
			{0x45, 5, 0, 0x00001fff},
			{0xb1, 0, 0, 0x0000000e},
			{0x50, 0, 0, 0x0000001b},
			{0x54, 0, 0, 0x00000012},
			{0x15, 0, 1, 0x00000010},
			{0x6, 0, 0, 0x0000ffff},
			{0x6, 0, 0, 0x00000000},
		}, false, true},

		// {"udp", false, false},
		// tcpdump -dd 'udp'
		{[]BPFInstruction{
			{0x28, 0, 0, 0x0000000c},
			{0x15, 0, 5, 0x000086dd},
			{0x30, 0, 0, 0x00000014},
			{0x15, 6, 0, 0x00000011},
			{0x15, 0, 6, 0x0000002c},
			{0x30, 0, 0, 0x00000036},
			{0x15, 3, 4, 0x00000011},
			{0x15, 0, 3, 0x00000800},
			{0x30, 0, 0, 0x00000017},
			{0x15, 0, 1, 0x00000011},
			{0x6, 0, 0, 0x0000ffff},
			{0x6, 0, 0, 0x00000000},
		}, false, false},

		{oversizedBpfInstructionBuffer[:], true, false},
	} {
		cntr++
		data, ci, err := handle.ReadPacketData()
		if err != nil {
			t.Fatal(err)
		}
		t.Log("Testing BpfInstruction filter", cntr)
		if bpf, err := handle.NewBPFInstructionFilter(expected.BpfInstruction); err != nil {
			if !expected.Error {
				t.Error(err, "while compiling filter was unexpected")
			}
		} else if expected.Error {
			t.Error("expected error but didn't see one")
		} else if matches := bpf.Matches(ci, data); matches != expected.Result {
			t.Error("Filter result was", matches, "but should be", expected.Result)
		}
	}
}

func ExampleBPF() {
	handle, err := OpenOffline("test_ethernet.pcap")
	if err != nil {
		log.Fatal(err)
	}
	synack, err := handle.NewBPF("tcp[tcpflags] & (tcp-syn|tcp-ack) == (tcp-syn|tcp-ack)")
	if err != nil {
		log.Fatal(err)
	}
	syn, err := handle.NewBPF("tcp[tcpflags] & (tcp-syn|tcp-ack) == tcp-syn")
	if err != nil {
		log.Fatal(err)
	}
	for {
		data, ci, err := handle.ReadPacketData()
		switch {
		case err == io.EOF:
			return
		case err != nil:
			log.Fatal(err)
		case synack.Matches(ci, data):
			fmt.Println("SYN/ACK packet")
		case syn.Matches(ci, data):
			fmt.Println("SYN packet")
		default:
			fmt.Println("SYN flag not set")
		}
	}
	// Output:
	// SYN packet
	// SYN/ACK packet
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
	// SYN flag not set
}
