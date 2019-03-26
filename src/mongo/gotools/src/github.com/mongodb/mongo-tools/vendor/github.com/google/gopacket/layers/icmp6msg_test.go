// Copyright 2012, Google, Inc. All rights reserved.
// Copyright 2009-2011 Andreas Krennmair. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"github.com/google/gopacket"
	"testing"
)

// testPacketICMPv6RouterAdvertisement is the packet:
// 23:34:40.014307 IP6 (class 0xe0, hlim 255, next-header ICMPv6 (58) payload length: 64) fe80::c000:54ff:fef5:0 > ip6-allnodes: [icmp6 sum ok] ICMP6, router advertisement, length 64
//         hop limit 64, Flags [none], pref medium, router lifetime 1800s, reachable time 0s, retrans time 0s
//           source link-address option (1), length 8 (1): c2:00:54:f5:00:00
//             0x0000:  c200 54f5 0000
//           mtu option (5), length 8 (1):  1500
//             0x0000:  0000 0000 05dc
//           prefix info option (3), length 32 (4): 2001:db8:0:1::/64, Flags [onlink, auto], valid time 2592000s, pref. time 604800s
//             0x0000:  40c0 0027 8d00 0009 3a80 0000 0000 2001
//             0x0010:  0db8 0000 0001 0000 0000 0000 0000
//         0x0000:  3333 0000 0001 c200 54f5 0000 86dd 6e00  33......T.....n.
//         0x0010:  0000 0040 3aff fe80 0000 0000 0000 c000  ...@:...........
//         0x0020:  54ff fef5 0000 ff02 0000 0000 0000 0000  T...............
//         0x0030:  0000 0000 0001 8600 c4fe 4000 0708 0000  ..........@.....
//         0x0040:  0000 0000 0000 0101 c200 54f5 0000 0501  ..........T.....
//         0x0050:  0000 0000 05dc 0304 40c0 0027 8d00 0009  ........@..'....
//         0x0060:  3a80 0000 0000 2001 0db8 0000 0001 0000  :...............
//         0x0070:  0000 0000 0000
var testPacketICMPv6RouterAdvertisement = []byte{
	0x33, 0x33, 0x00, 0x00, 0x00, 0x01, 0xc2, 0x00, 0x54, 0xf5, 0x00, 0x00, 0x86, 0xdd, 0x6e, 0x00,
	0x00, 0x00, 0x00, 0x40, 0x3a, 0xff, 0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00,
	0x54, 0xff, 0xfe, 0xf5, 0x00, 0x00, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x86, 0x00, 0xc4, 0xfe, 0x40, 0x00, 0x07, 0x08, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0xc2, 0x00, 0x54, 0xf5, 0x00, 0x00, 0x05, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x05, 0xdc, 0x03, 0x04, 0x40, 0xc0, 0x00, 0x27, 0x8d, 0x00, 0x00, 0x09,
	0x3a, 0x80, 0x00, 0x00, 0x00, 0x00, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
}

func TestPacketICMPv6RouterAdvertisement(t *testing.T) {
	p := gopacket.NewPacket(testPacketICMPv6RouterAdvertisement, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv6, LayerTypeICMPv6, LayerTypeICMPv6RouterAdvertisement}, t)
}

// testPacketICMPv6NeighborSolicitation is the packet:
// 23:34:39.647300 IP6 (hlim 255, next-header ICMPv6 (58) payload length: 24) :: > ff02::1:ff0e:4c67: [icmp6 sum ok] ICMP6, neighbor solicitation, length 24, who has fe80::20c:29ff:fe0e:4c67
//         0x0000:  3333 ff0e 4c67 000c 290e 4c67 86dd 6000  33..Lg..).Lg..`.
//         0x0010:  0000 0018 3aff 0000 0000 0000 0000 0000  ....:...........
//         0x0020:  0000 0000 0000 ff02 0000 0000 0000 0000  ................
//         0x0030:  0001 ff0e 4c67 8700 b930 0000 0000 fe80  ....Lg...0......
//         0x0040:  0000 0000 0000 020c 29ff fe0e 4c67       ........)...Lg
var testPacketICMPv6NeighborSolicitation = []byte{
	0x33, 0x33, 0xff, 0x0e, 0x4c, 0x67, 0x00, 0x0c, 0x29, 0x0e, 0x4c, 0x67, 0x86, 0xdd, 0x60, 0x00,
	0x00, 0x00, 0x00, 0x18, 0x3a, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x01, 0xff, 0x0e, 0x4c, 0x67, 0x87, 0x00, 0xb9, 0x30, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x80,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x0c, 0x29, 0xff, 0xfe, 0x0e, 0x4c, 0x67,
}

func TestPacketICMPv6NeighborSolicitation(t *testing.T) {
	p := gopacket.NewPacket(testPacketICMPv6NeighborSolicitation, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv6, LayerTypeICMPv6, LayerTypeICMPv6NeighborSolicitation}, t)
}
