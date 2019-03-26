// Copyright 2018 GoPacket Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"testing"

	"github.com/google/gopacket"
)

// Adapted from https://github.com/the-tcpdump-group/tcpdump/blob/master/tests/icmpv6.pcap
// BSD licensed content
//
// Frame 3: 90 bytes on wire (720 bits), 90 bytes captured (720 bits)
// Ethernet II, Src: JuniperN_0c:d4:e8 (b0:a8:6e:0c:d4:e8), Dst: IPv6mcast_01 (33:33:00:00:00:01)
// Internet Protocol Version 6, Src: fe80::b2a8:6eff:fe0c:d4e8, Dst: ff02::1
//  0110 .... = Version: 6
//  .... 0000 0000 .... .... .... .... .... = Traffic Class: 0x00 (DSCP: CS0, ECN: Not-ECT)
//  .... .... .... 0000 0000 0000 0000 0000 = Flow Label: 0x00000
//  Payload Length: 36
//  Next Header: IPv6 Hop-by-Hop Option (0)
//  Hop Limit: 1
//  Source: fe80::b2a8:6eff:fe0c:d4e8
//  Destination: ff02::1
//  [Source SA MAC: JuniperN_0c:d4:e8 (b0:a8:6e:0c:d4:e8)]
//  IPv6 Hop-by-Hop Option
// Internet Control Message Protocol v6
//  Type: Multicast Listener Query (130)
//  Code: 0
//  Checksum: 0x623a [correct]
//  [Checksum Status: Good]
//  Maximum Response Code: 10000
//  Reserved: 0000
//  Multicast Address: ::
var testPacketMulticastListenerQueryMessageV1 = []byte{
	0x33, 0x33, 0x00, 0x00, 0x00, 0x01, 0xb0, 0xa8, 0x6e, 0x0c, 0xd4, 0xe8, 0x86, 0xdd, 0x60, 0x00,
	0x00, 0x00, 0x00, 0x24, 0x00, 0x01, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb2, 0xa8,
	0x6e, 0xff, 0xfe, 0x0c, 0xd4, 0xe8, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3a, 0x00, 0x05, 0x02, 0x00, 0x00, 0x01, 0x00, 0x82, 0x00,
	0x62, 0x3a, 0x27, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
}

func TestPacketMulticastListenerQueryMessageV1(t *testing.T) {
	p := gopacket.NewPacket(testPacketMulticastListenerQueryMessageV1, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv6, LayerTypeIPv6HopByHop, LayerTypeICMPv6, LayerTypeMLDv1MulticastListenerQuery}, t)
	// See https://github.com/google/gopacket/issues/517
	// checkSerialization(p, t)
}

// Adapted from https://github.com/the-tcpdump-group/tcpdump/blob/master/tests/icmpv6.pcap
// BSD licensed content
//
// Ethernet II, Src: JuniperN_0c:d4:e8 (b0:a8:6e:0c:d4:e8), Dst: IPv6mcast_01 (33:33:00:00:00:01)
// Internet Protocol Version 6, Src: fe80::b2a8:6eff:fe0c:d4e8, Dst: ff02::1
//  0110 .... = Version: 6
//  .... 0000 0000 .... .... .... .... .... = Traffic Class: 0x00 (DSCP: CS0, ECN: Not-ECT)
//  .... .... .... 0000 0000 0000 0000 0000 = Flow Label: 0x00000
//  Payload Length: 36
//  Next Header: IPv6 Hop-by-Hop Option (0)
//  Hop Limit: 1
//  Source: fe80::b2a8:6eff:fe0c:d4e8
//  Destination: ff02::1
//  [Source SA MAC: JuniperN_0c:d4:e8 (b0:a8:6e:0c:d4:e8)]
//  IPv6 Hop-by-Hop Option
// Internet Control Message Protocol v6
//  Type: Multicast Listener Report (131)
//  Code: 0
//  Checksum: 0x623a [incorrect]
//  [Checksum Status: Invalid]
//  Maximum Response Code: 10000
//  Reserved: 0000
//  Multicast Address: ff02::db8:1122:3344
var testPacketMulticastListenerReportMessageV1 = []byte{
	0x33, 0x33, 0x00, 0x00, 0x00, 0x01, 0xb0, 0xa8, 0x6e, 0x0c, 0xd4, 0xe8, 0x86, 0xdd, 0x60, 0x00,
	0x00, 0x00, 0x00, 0x24, 0x00, 0x01, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb2, 0xa8,
	0x6e, 0xff, 0xfe, 0x0c, 0xd4, 0xe8, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3a, 0x00, 0x05, 0x02, 0x00, 0x00, 0x01, 0x00, 0x83, 0x00,
	0x62, 0x3a, 0x27, 0x10, 0x00, 0x00, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0d, 0xb8, 0x11, 0x22, 0x33, 0x44,
}

func TestPacketMulticastListenerReportMessageV1(t *testing.T) {
	p := gopacket.NewPacket(testPacketMulticastListenerReportMessageV1, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv6, LayerTypeIPv6HopByHop, LayerTypeICMPv6, LayerTypeMLDv1MulticastListenerReport}, t)
	// See https://github.com/google/gopacket/issues/517
	// checkSerialization(p, t)
}

// Adapted from https://github.com/the-tcpdump-group/tcpdump/blob/master/tests/icmpv6.pcap
// BSD licensed content
//
// Ethernet II, Src: JuniperN_0c:d4:e8 (b0:a8:6e:0c:d4:e8), Dst: IPv6mcast_01 (33:33:00:00:00:01)
// Internet Protocol Version 6, Src: fe80::b2a8:6eff:fe0c:d4e8, Dst: ff02::1
//  0110 .... = Version: 6
//  .... 0000 0000 .... .... .... .... .... = Traffic Class: 0x00 (DSCP: CS0, ECN: Not-ECT)
//  .... .... .... 0000 0000 0000 0000 0000 = Flow Label: 0x00000
//  Payload Length: 36
//  Next Header: IPv6 Hop-by-Hop Option (0)
//  Hop Limit: 1
//  Source: fe80::b2a8:6eff:fe0c:d4e8
//  Destination: ff02::1
//  [Source SA MAC: JuniperN_0c:d4:e8 (b0:a8:6e:0c:d4:e8)]
//  IPv6 Hop-by-Hop Option
// Internet Control Message Protocol v6
//  Type: Multicast Listener Done (132)
//  Code: 0
//  Checksum: 0x623a [incorrect]
//  [Checksum Status: Invalid]
//  Maximum Response Code: 10000
//  Reserved: 0000
//  Multicast Address: ff02::db8:1122:3344
var testPacketMulticastListenerDoneMessageV1 = []byte{
	0x33, 0x33, 0x00, 0x00, 0x00, 0x01, 0xb0, 0xa8, 0x6e, 0x0c, 0xd4, 0xe8, 0x86, 0xdd, 0x60, 0x00,
	0x00, 0x00, 0x00, 0x24, 0x00, 0x01, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb2, 0xa8,
	0x6e, 0xff, 0xfe, 0x0c, 0xd4, 0xe8, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3a, 0x00, 0x05, 0x02, 0x00, 0x00, 0x01, 0x00, 0x84, 0x00,
	0x62, 0x3a, 0x27, 0x10, 0x00, 0x00, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0d, 0xb8, 0x11, 0x22, 0x33, 0x44,
}

func TestPacketMulticastListenerDoneMessageV1(t *testing.T) {
	p := gopacket.NewPacket(testPacketMulticastListenerDoneMessageV1, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv6, LayerTypeIPv6HopByHop, LayerTypeICMPv6, LayerTypeMLDv1MulticastListenerDone}, t)
	// See https://github.com/google/gopacket/issues/517
	// checkSerialization(p, t)
}
