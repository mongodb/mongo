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

var icmp6NeighborAnnouncementData = []byte{
	// Ethernet layer
	0x00, 0x1F, 0xCA, 0xB3, 0x76, 0x40, // destination
	0x24, 0xBE, 0x05, 0x27, 0x0B, 0x17, // source
	0x86, 0xDD, // type IPv6

	// IPv6 layer
	0x60, 0x00, 0x00, 0x00, // version; traffic class; flow label
	0x00, 0x18, // payload length?
	0x3A,                                                                                           // Next Header - IPv6-ICMP
	0xFF,                                                                                           // Hop Limit
	0x26, 0x20, 0x00, 0x00, 0x10, 0x05, 0x00, 0x00, 0x26, 0xBE, 0x05, 0xFF, 0xFE, 0x27, 0x0B, 0x17, // source
	0xFE, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x1F, 0xCA, 0xFF, 0xFE, 0xB3, 0x76, 0x40, // destination

	// ICMPv6 layer
	0x88, 0x00, // ICMP type 136, code 0

	0x1E, 0xD6, // checksum
	0x40, 0x00, 0x00, 0x00, // flags & reserved
	0x26, 0x20, 0x00, 0x00, 0x10, 0x05, 0x00, 0x00, 0x26, 0xBE, 0x05, 0xFF, 0xFE, 0x27, 0x0B, 0x17, // target address
}

var icmp6RouterAdvertisementData = []byte{
	// Ethernet layer
	0x33, 0x33, 0x00, 0x00, 0x00, 0x01, // destination,
	0xde, 0x42, 0x72, 0xb0, 0x1e, 0xf4, // source
	0x86, 0xdd,

	// IPv6 layer
	0x60, 0x00, 0x00, 0x00, // version; traffic class; flow label
	0x00, 0x20, // payload length?
	0x3a,                                                                                           // Next Header - IPv6-ICMP
	0xff,                                                                                           // Hop Limit
	0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdc, 0x42, 0x72, 0xff, 0xfe, 0xb0, 0x1e, 0xf4, // source,
	0xff, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, // destination

	// ICMPv6 layer
	0x86, 0x00, // ICMP type 134, code 0

	0x4c, 0x6b, // checksum
	0x40,       // current hop limit
	0x00,       // flags & reserves
	0x07, 0x08, // router lifetime
	0x00, 0x00, 0x00, 0x00, // reachable time
	0x00, 0x00, 0x00, 0x00, //retrans time
	0x01, 0x01, 0xde, 0x42, 0x72, 0xb0, 0x1e, 0xf4, // source link layer address
	0x05, 0x01, 0x00, 0x00, 0x00, 0x00, 0x05, 0xdc, // MTU option
}

func TestPacketICMPv6NeighborAnnouncementFlags(t *testing.T) {
	var ethLayer Ethernet
	var ipV6Layer IPv6
	var icmpLayer ICMPv6

	var icmpNeighAdvLayer ICMPv6NeighborAdvertisement

	parser := gopacket.NewDecodingLayerParser(LayerTypeEthernet, &ethLayer, &ipV6Layer, &icmpLayer)
	parser.IgnoreUnsupported = true

	respLayers := make([]gopacket.LayerType, 0)
	err := parser.DecodeLayers(icmp6NeighborAnnouncementData, &respLayers)

	if err != nil {
		t.Errorf("error decoding layers %s", err)
		return
	}

	err = icmpNeighAdvLayer.DecodeFromBytes(icmpLayer.LayerPayload(), gopacket.NilDecodeFeedback)
	if err != nil {
		t.Errorf("Error while Decodeing From Bytes: %s", err)
		return
	}

	if icmpNeighAdvLayer.Router() {
		t.Errorf("This Neighbor Advertisement message's Router flag should not be set")
	}
	if !icmpNeighAdvLayer.Solicited() {
		t.Errorf("This Neighbor Advertisement message's Solicited flag should be set")
	}
	if icmpNeighAdvLayer.Override() {
		t.Errorf("This Neighbor Advertisement message's Override bit should not be set")
	}
}

func TestPacketICMPv6RouterAnnouncementFlags(t *testing.T) {
	var ethLayer Ethernet
	var ipV6Layer IPv6
	var icmpLayer ICMPv6

	var icmpRouterAdvLayer ICMPv6RouterAdvertisement

	parser := gopacket.NewDecodingLayerParser(LayerTypeEthernet, &ethLayer, &ipV6Layer, &icmpLayer)
	parser.IgnoreUnsupported = true

	respLayers := make([]gopacket.LayerType, 0)
	err := parser.DecodeLayers(icmp6RouterAdvertisementData, &respLayers)

	if err != nil {
		t.Errorf("error decoding layers %s", err)
		return
	}

	err = icmpRouterAdvLayer.DecodeFromBytes(icmpLayer.LayerPayload(), gopacket.NilDecodeFeedback)
	if err != nil {
		t.Errorf("Error while Decodeing From Bytes: %s", err)
		return
	}

	if icmpRouterAdvLayer.ManagedAddressConfig() {
		t.Errorf("This Router Advertisement message's ManagedAddressConfig flag should not be set")
	}
	if icmpRouterAdvLayer.OtherConfig() {
		t.Errorf("This Router Advertisement message's OtherConfig flag should not be set")
	}
}
