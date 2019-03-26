// Copyright 2016 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.
package layers

import (
	"github.com/google/gopacket"
	"testing"
)

// vrrpPacketPriority100 is the packet:
//   06:12:21.813317 IP 192.168.0.30 > 224.0.0.18: VRRPv2, Advertisement, vrid 1, prio 100, authtype none, intvl 1s, length 20
//   	0x0000:  0100 5e00 0012 0000 5e00 0101 0800 45c0  ..^.....^.....E.
//   	0x0010:  0028 0000 0000 ff70 19cd c0a8 001e e000  .(.....p........
//   	0x0020:  0012 2101 6401 0001 ba52 c0a8 0001 0000  ..!.d....R......
//   	0x0030:  0000 0000 0000 0000 0000 0000            ............
var vrrpPacketPriority100 = []byte{
	0x01, 0x00, 0x5e, 0x00, 0x00, 0x12, 0x00, 0x00, 0x5e, 0x00, 0x01, 0x01, 0x08, 0x00, 0x45, 0xc0,
	0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0xff, 0x70, 0x19, 0xcd, 0xc0, 0xa8, 0x00, 0x1e, 0xe0, 0x00,
	0x00, 0x12, 0x21, 0x01, 0x64, 0x01, 0x00, 0x01, 0xba, 0x52, 0xc0, 0xa8, 0x00, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
}

func TestVRRPPacketPacket0(t *testing.T) {
	p := gopacket.NewPacket(vrrpPacketPriority100, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeVRRP}, t)

	// Version=2 Type=VRRPv2 Advertisement VirtualRtrID=1 Priority=100
	vrrp := p.Layer(LayerTypeVRRP).(*VRRPv2)
	if vrrp.Version != 2 {
		t.Fatalf("Unable to decode VRRPv2 version. Received %d, expected %d", vrrp.Version, 2)
	}

	if vrrp.Type != 1 {
		t.Fatalf("Unable to decode VRRPv2 type. Received %d, expected %d", vrrp.Type, 1)
	}

	if vrrp.Priority != 100 {
		t.Fatalf("Unable to decode VRRPv2 priority. Received %d, expected %d", vrrp.Priority, 100)
	}

	if vrrp.Checksum != 47698 {
		t.Fatalf("Unable to decode VRRPv2 checksum. Received %d, expected %d", vrrp.Checksum, 47698)
	}
}
func BenchmarkDecodeVRRPPacket0(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(vrrpPacketPriority100, LayerTypeEthernet, gopacket.NoCopy)
	}
}
