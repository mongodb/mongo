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
// Ethernet II, Src: JuniperN_0c:d4:e8 (b0:a8:6e:0c:d4:e8), Dst: IPv6mcast_01 (33:33:00:00:00:01)
// Internet Protocol Version 6, Src: fe80::b2a8:6eff:fe0c:d4e8, Dst: ff02::1
//     0110 .... = Version: 6
//     .... 0000 0000 .... .... .... .... .... = Traffic Class: 0x00 (DSCP: CS0, ECN: Not-ECT)
//     .... .... .... 0000 0000 0000 0000 0000 = Flow Label: 0x00000
//     Payload Length: 36
//     Next Header: IPv6 Hop-by-Hop Option (0)
//     Hop Limit: 1
//     Source: fe80::b2a8:6eff:fe0c:d4e8
//     Destination: ff02::1
//     [Source SA MAC: JuniperN_0c:d4:e8 (b0:a8:6e:0c:d4:e8)]
//     IPv6 Hop-by-Hop Option
// Internet Control Message Protocol v6
//     Type: Multicast Listener Query (130)
//     Code: 0
//     Checksum: 0x623a [correct]
//     [Checksum Status: Good]
//     Maximum Response Code: 10000
//     Reserved: 0000
//     Multicast Address: ::
//     Flags: 0x02
//         .... 0... = Suppress Router-Side Processing: False
//         .... .010 = QRV (Querier's Robustness Variable): 2
//         0000 .... = Reserved: 0
//     QQIC (Querier's Query Interval Code): 60
//     Number of Sources: 0
var testPacketMulticastListenerQueryMessageV2 = []byte{
	0x33, 0x33, 0x00, 0x00, 0x00, 0x01, 0xb0, 0xa8, 0x6e, 0x0c, 0xd4, 0xe8, 0x86, 0xdd, 0x60, 0x00,
	0x00, 0x00, 0x00, 0x24, 0x00, 0x01, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb2, 0xa8,
	0x6e, 0xff, 0xfe, 0x0c, 0xd4, 0xe8, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x3a, 0x00, 0x05, 0x02, 0x00, 0x00, 0x01, 0x00, 0x82, 0x00,
	0x62, 0x3a, 0x27, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x3c, 0x00, 0x00,
}

func TestPacketMulticastListenerQueryMessageV2(t *testing.T) {
	p := gopacket.NewPacket(testPacketMulticastListenerQueryMessageV2, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{
		LayerTypeEthernet,
		LayerTypeIPv6,
		LayerTypeIPv6HopByHop,
		LayerTypeICMPv6,
		LayerTypeMLDv2MulticastListenerQuery}, t)
	// See https://github.com/google/gopacket/issues/517
	// checkSerialization(p, t)
}

// Adapted from https://github.com/the-tcpdump-group/tcpdump/blob/master/tests/icmpv6.pcap
// BSD licensed content
//
// Frame 4: 150 bytes on wire (1200 bits), 150 bytes captured (1200 bits)
// Ethernet II, Src: IntelCor_cc:e5:46 (00:15:17:cc:e5:46), Dst: IPv6mcast_16 (33:33:00:00:00:16)
// Internet Protocol Version 6, Src: fe80::215:17ff:fecc:e546, Dst: ff02::16
//     0110 .... = Version: 6
//     .... 0000 0000 .... .... .... .... .... = Traffic Class: 0x00 (DSCP: CS0, ECN: Not-ECT)
//     .... .... .... 0000 0000 0000 0000 0000 = Flow Label: 0x00000
//     Payload Length: 96
//     Next Header: IPv6 Hop-by-Hop Option (0)
//     Hop Limit: 1
//     Source: fe80::215:17ff:fecc:e546
//     Destination: ff02::16
//     [Source SA MAC: IntelCor_cc:e5:46 (00:15:17:cc:e5:46)]
//     IPv6 Hop-by-Hop Option
// Internet Control Message Protocol v6
//     Type: Multicast Listener Report Message v2 (143)
//     Code: 0
//     Checksum: 0x2a0e [correct]
//     [Checksum Status: Good]
//     Reserved: 0000
//     Number of Multicast Address Records: 4
//     Multicast Address Record Exclude: ff02::db8:1122:3344
//         Record Type: Exclude (2)
//         Aux Data Len: 0
//         Number of Sources: 0
//         Multicast Address: ff02::db8:1122:3344
//     Multicast Address Record Exclude: ff02::1:ffcc:e546
//         Record Type: Exclude (2)
//         Aux Data Len: 0
//         Number of Sources: 0
//         Multicast Address: ff02::1:ffcc:e546
//     Multicast Address Record Exclude: ff02::1:ffa7:10ad
//         Record Type: Exclude (2)
//         Aux Data Len: 0
//         Number of Sources: 0
//         Multicast Address: ff02::1:ffa7:10ad
//     Multicast Address Record Exclude: ff02::1:ff00:2
//         Record Type: Exclude (2)
//         Aux Data Len: 0
//         Number of Sources: 0
//         Multicast Address: ff02::1:ff00:2
var testPacketMulticastListenerReportMessageV2 = []byte{
	0x33, 0x33, 0x00, 0x00, 0x00, 0x16, 0x00, 0x15, 0x17, 0xcc, 0xe5, 0x46, 0x86, 0xdd, 0x60, 0x00,
	0x00, 0x00, 0x00, 0x60, 0x00, 0x01, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x15,
	0x17, 0xff, 0xfe, 0xcc, 0xe5, 0x46, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x3a, 0x00, 0x05, 0x02, 0x00, 0x00, 0x01, 0x00, 0x8f, 0x00,
	0x2a, 0x0e, 0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x00, 0x00, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x0d, 0xb8, 0x11, 0x22, 0x33, 0x44, 0x02, 0x00, 0x00, 0x00, 0xff, 0x02,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xcc, 0xe5, 0x46, 0x02, 0x00,
	0x00, 0x00, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xff, 0xa7,
	0x10, 0xad, 0x02, 0x00, 0x00, 0x00, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x01, 0xff, 0x00, 0x00, 0x02,
}

func TestPacketMulticastListenerReportMessageV2(t *testing.T) {
	p := gopacket.NewPacket(testPacketMulticastListenerReportMessageV2, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{
		LayerTypeEthernet,
		LayerTypeIPv6,
		LayerTypeIPv6HopByHop,
		LayerTypeICMPv6,
		LayerTypeMLDv2MulticastListenerReport}, t)
	// See https://github.com/google/gopacket/issues/517
	// checkSerialization(p, t)
}
