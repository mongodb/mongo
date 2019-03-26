// Copyright 2016, Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"testing"

	"github.com/google/gopacket"
)

// igmpv1MembershipReportPacket is the packet:
//   02:45:36.033916 IP 10.60.0.132 > 224.0.1.60: igmp v1 report 224.0.1.60
//   	0x0000:  0100 5e00 013c 0030 c1bf 5755 0800 4500  ..^..<.0..WU..E.
//   	0x0010:  001c 6a7f 0000 0102 6365 0a3c 0084 e000  ..j.....ce.<....
//   	0x0020:  013c 1200 0cc3 e000 013c 0000 0000 0000  .<.......<......
//   	0x0030:  ffff ffff ffff 0452 0000 0000            .......R....
var igmpv1MembershipReportPacket = []byte{
	0x01, 0x00, 0x5e, 0x00, 0x01, 0x3c, 0x00, 0x30, 0xc1, 0xbf, 0x57, 0x55, 0x08, 0x00, 0x45, 0x00,
	0x00, 0x1c, 0x6a, 0x7f, 0x00, 0x00, 0x01, 0x02, 0x63, 0x65, 0x0a, 0x3c, 0x00, 0x84, 0xe0, 0x00,
	0x01, 0x3c, 0x12, 0x00, 0x0c, 0xc3, 0xe0, 0x00, 0x01, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x04, 0x52, 0x00, 0x00, 0x00, 0x00,
}

func TestIGMPv1MembershipReportPacket(t *testing.T) {
	p := gopacket.NewPacket(igmpv1MembershipReportPacket, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeIGMP}, t)

	igmp := p.Layer(LayerTypeIGMP).(*IGMPv1or2)
	if igmp.Type != IGMPMembershipReportV1 {
		t.Fatal("Invalid IGMP type")
	}
}

func BenchmarkDecodeigmpv1MembershipReportPacket(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(igmpv1MembershipReportPacket, LinkTypeEthernet, gopacket.NoCopy)
	}
}

// igmpv2MembershipQueryPacket is the packet:
//   02:45:28.071636 IP 10.60.0.189 > 224.0.0.1: igmp query v2
//   	0x0000:  0100 5e00 0001 0001 636f c800 0800 45c0  ..^.....co....E.
//   	0x0010:  001c 0153 0000 0102 ccd3 0a3c 00bd e000  ...S.......<....
//   	0x0020:  0001 1164 ee9b 0000 0000 0000 0000 0000  ...d............
//   	0x0030:  0000 0000 0000 0000 0000 0000            ............
var igmpv2MembershipQueryPacket = []byte{
	0x01, 0x00, 0x5e, 0x00, 0x00, 0x01, 0x00, 0x01, 0x63, 0x6f, 0xc8, 0x00, 0x08, 0x00, 0x45, 0xc0,
	0x00, 0x1c, 0x01, 0x53, 0x00, 0x00, 0x01, 0x02, 0xcc, 0xd3, 0x0a, 0x3c, 0x00, 0xbd, 0xe0, 0x00,
	0x00, 0x01, 0x11, 0x64, 0xee, 0x9b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
}

func TestIGMPv2MembershipQuery(t *testing.T) {
	p := gopacket.NewPacket(igmpv2MembershipQueryPacket, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeIGMP}, t)

	igmp := p.Layer(LayerTypeIGMP).(*IGMPv1or2)
	if igmp.Type != IGMPMembershipQuery {
		t.Fatal("Invalid IGMP type")
	}
}
func BenchmarkDecodeigmpv2MembershipQueryPacket(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(igmpv2MembershipQueryPacket, LinkTypeEthernet, gopacket.NoCopy)
	}
}

// igmpv2MembershipReportPacket is the packet:
//   02:47:32.417288 IP 10.60.5.103 > 239.255.255.253: igmp v2 report 239.255.255.253
//   	0x0000:  0100 5e7f fffd 0015 58dc d9f6 0800 4600  ..^.....X.....F.
//   	0x0010:  0020 79f0 0000 0102 ab47 0a3c 0567 efff  ..y......G.<.g..
//   	0x0020:  fffd 9404 0000 1600 fa01 efff fffd 0000  ................
//   	0x0030:  0000 0000 0000 0000 0000 0000            ............
var igmpv2MembershipReportPacket = []byte{
	0x01, 0x00, 0x5e, 0x7f, 0xff, 0xfd, 0x00, 0x15, 0x58, 0xdc, 0xd9, 0xf6, 0x08, 0x00, 0x46, 0x00,
	0x00, 0x20, 0x79, 0xf0, 0x00, 0x00, 0x01, 0x02, 0xab, 0x47, 0x0a, 0x3c, 0x05, 0x67, 0xef, 0xff,
	0xff, 0xfd, 0x94, 0x04, 0x00, 0x00, 0x16, 0x00, 0xfa, 0x01, 0xef, 0xff, 0xff, 0xfd, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
}

func TestIGMPv2MembershipReport(t *testing.T) {
	p := gopacket.NewPacket(igmpv2MembershipReportPacket, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeIGMP}, t)

	igmp := p.Layer(LayerTypeIGMP).(*IGMPv1or2)
	if igmp.Type != IGMPMembershipReportV2 {
		t.Fatal("Invalid IGMP type")
	}
}
func BenchmarkDecodeigmpv2MembershipReportPacket(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(igmpv2MembershipReportPacket, LinkTypeEthernet, gopacket.NoCopy)
	}
}

// igmp3v3MembershipQueryPacket is the packet:
//   10:07:30.488511 IP 192.168.1.254 > 224.0.0.1: igmp query v3 [max resp time 2.4s]
//      0x0000:  0100 5e00 0001 0026 446c 1eda 0800 46c0  ..^....&Dl....F.
//      0x0010:  0024 17f1 4000 0102 297b c0a8 01fe e000  .$..@...){......
//      0x0020:  0001 9404 0000 1118 ecd3 0000 0000 0214  ................
//      0x0030:  0000 0000 0000 0000 0000 0000            ............
var igmp3v3MembershipQueryPacket = []byte{
	0x01, 0x00, 0x5e, 0x00, 0x00, 0x01, 0x00, 0x26, 0x44, 0x6c, 0x1e, 0xda, 0x08, 0x00, 0x46, 0xc0,
	0x00, 0x24, 0x17, 0xf1, 0x40, 0x00, 0x01, 0x02, 0x29, 0x7b, 0xc0, 0xa8, 0x01, 0xfe, 0xe0, 0x00,
	0x00, 0x01, 0x94, 0x04, 0x00, 0x00, 0x11, 0x18, 0xec, 0xd3, 0x00, 0x00, 0x00, 0x00, 0x02, 0x14,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
}

func TestIGMPv3MembershipQuery(t *testing.T) {
	p := gopacket.NewPacket(igmp3v3MembershipQueryPacket, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeIGMP}, t)

	igmp := p.Layer(LayerTypeIGMP).(*IGMP)
	if igmp.Type != IGMPMembershipQuery {
		t.Fatal("Invalid IGMP type")
	}
}

func BenchmarkDecodeigmp3v3MembershipQueryPacket(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(igmp3v3MembershipQueryPacket, LinkTypeEthernet, gopacket.NoCopy)
	}
}

// igmpv3MembershipReport2Records is the packet:
//   10:07:29.756202 IP 192.168.1.66 > 224.0.0.22: igmp v3 report, 2 group record(s)
//      0x0000:  0100 5e00 0016 0025 2e51 c381 0800 4658  ..^....%.Q....FX
//      0x0010:  0030 013c 0000 0102 8133 c0a8 0142 e000  .0.<.....3...B..
//      0x0020:  0016 9404 0000 2200 f33c 0000 0002 0200  ......"..<......
//      0x0030:  0000 efc3 0702 0200 0000 efff fffa       ..............
var igmpv3MembershipReport2Records = []byte{
	0x01, 0x00, 0x5e, 0x00, 0x00, 0x16, 0x00, 0x25, 0x2e, 0x51, 0xc3, 0x81, 0x08, 0x00, 0x46, 0x58,
	0x00, 0x30, 0x01, 0x3c, 0x00, 0x00, 0x01, 0x02, 0x81, 0x33, 0xc0, 0xa8, 0x01, 0x42, 0xe0, 0x00,
	0x00, 0x16, 0x94, 0x04, 0x00, 0x00, 0x22, 0x00, 0xf3, 0x3c, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00,
	0x00, 0x00, 0xef, 0xc3, 0x07, 0x02, 0x02, 0x00, 0x00, 0x00, 0xef, 0xff, 0xff, 0xfa,
}

func TestIGMPv3MembershipReport2Records(t *testing.T) {
	p := gopacket.NewPacket(igmpv3MembershipReport2Records, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeIGMP}, t)

	igmp := p.Layer(LayerTypeIGMP).(*IGMP)
	if igmp.Type != IGMPMembershipReportV3 {
		t.Fatal("Invalid IGMP type")
	}
}

func BenchmarkDecodeigmpv3MembershipReport2Records(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(igmpv3MembershipReport2Records, LinkTypeEthernet, gopacket.NoCopy)
	}
}
