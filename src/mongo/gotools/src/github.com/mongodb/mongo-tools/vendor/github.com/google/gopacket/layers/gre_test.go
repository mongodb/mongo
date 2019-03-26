// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.
package layers

import (
	"fmt"
	"net"
	"reflect"
	"testing"

	"github.com/google/gopacket"
)

// testPacketGRE is the packet:
//   15:08:08.003196 IP 192.168.1.1 > 192.168.1.2: GREv0, length 88: IP 172.16.1.1 > 172.16.2.1: ICMP echo request, id 4724, seq 1, length 64
//      0x0000:  3a56 6b69 595e 8e7a 12c3 a971 0800 4500  :VkiY^.z...q..E.
//      0x0010:  006c 843c 4000 402f 32d3 c0a8 0101 c0a8  .l.<@.@/2.......
//      0x0020:  0102 0000 0800 4500 0054 0488 4000 4001  ......E..T..@.@.
//      0x0030:  dafe ac10 0101 ac10 0201 0800 82c4 1274  ...............t
//      0x0040:  0001 c892 a354 0000 0000 380c 0000 0000  .....T....8.....
//      0x0050:  0000 1011 1213 1415 1617 1819 1a1b 1c1d  ................
//      0x0060:  1e1f 2021 2223 2425 2627 2829 2a2b 2c2d  ...!"#$%&'()*+,-
//      0x0070:  2e2f 3031 3233 3435 3637                 ./01234567
var testPacketGRE = []byte{
	0x3a, 0x56, 0x6b, 0x69, 0x59, 0x5e, 0x8e, 0x7a, 0x12, 0xc3, 0xa9, 0x71, 0x08, 0x00, 0x45, 0x00,
	0x00, 0x6c, 0x84, 0x3c, 0x40, 0x00, 0x40, 0x2f, 0x32, 0xd3, 0xc0, 0xa8, 0x01, 0x01, 0xc0, 0xa8,
	0x01, 0x02, 0x00, 0x00, 0x08, 0x00, 0x45, 0x00, 0x00, 0x54, 0x04, 0x88, 0x40, 0x00, 0x40, 0x01,
	0xda, 0xfe, 0xac, 0x10, 0x01, 0x01, 0xac, 0x10, 0x02, 0x01, 0x08, 0x00, 0x82, 0xc4, 0x12, 0x74,
	0x00, 0x01, 0xc8, 0x92, 0xa3, 0x54, 0x00, 0x00, 0x00, 0x00, 0x38, 0x0c, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
	0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
	0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
}

func TestPacketGRE(t *testing.T) {
	p := gopacket.NewPacket(testPacketGRE, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeGRE, LayerTypeIPv4, LayerTypeICMPv4, gopacket.LayerTypePayload}, t)
	if got, ok := p.Layer(LayerTypeGRE).(*GRE); ok {
		want := &GRE{
			BaseLayer: BaseLayer{testPacketGRE[34:38], testPacketGRE[38:]},
			Protocol:  EthernetTypeIPv4,
		}
		if !reflect.DeepEqual(want, got) {
			t.Errorf("GRE layer mismatch, \nwant %#v\ngot  %#v\n", want, got)
		}
	}
}

func BenchmarkDecodePacketGRE(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(testPacketGRE, LinkTypeEthernet, gopacket.NoCopy)
	}
}

var testIPv4OverGRE = []gopacket.SerializableLayer{
	&Ethernet{
		SrcMAC:       net.HardwareAddr{142, 122, 18, 195, 169, 113},
		DstMAC:       net.HardwareAddr{58, 86, 107, 105, 89, 94},
		EthernetType: EthernetTypeIPv4,
	},
	&IPv4{
		Version:  4,
		SrcIP:    net.IP{192, 168, 1, 1},
		DstIP:    net.IP{192, 168, 1, 2},
		Protocol: IPProtocolGRE,
		Flags:    IPv4DontFragment,
		TTL:      64,
		Id:       33852,
		IHL:      5,
	},
	&GRE{
		Protocol: EthernetTypeIPv4,
	},
	&IPv4{
		Version:  4,
		SrcIP:    net.IP{172, 16, 1, 1},
		DstIP:    net.IP{172, 16, 2, 1},
		Protocol: IPProtocolICMPv4,
		Flags:    IPv4DontFragment,
		TTL:      64,
		IHL:      5,
		Id:       1160,
	},
	&ICMPv4{
		TypeCode: CreateICMPv4TypeCode(ICMPv4TypeEchoRequest, 0),
		Id:       4724,
		Seq:      1,
	},
	gopacket.Payload{
		0xc8, 0x92, 0xa3, 0x54, 0x00, 0x00, 0x00, 0x00, 0x38, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
		0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	},
}

func TestIPv4OverGREEncode(t *testing.T) {
	b := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{
		ComputeChecksums: true,
		FixLengths:       true,
	}
	if err := gopacket.SerializeLayers(b, opts, testIPv4OverGRE...); err != nil {
		t.Errorf("Unable to serialize: %v", err)
	}
	p := gopacket.NewPacket(b.Bytes(), LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeGRE, LayerTypeIPv4, LayerTypeICMPv4, gopacket.LayerTypePayload}, t)
	if got, want := b.Bytes(), testPacketGRE; !reflect.DeepEqual(want, got) {
		t.Errorf("Encoding mismatch, \nwant: %v\ngot %v\n", want, got)
	}
}

func BenchmarkEncodePacketGRE(b *testing.B) {
	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{
		ComputeChecksums: true,
		FixLengths:       true,
	}
	for i := 0; i < b.N; i++ {
		gopacket.SerializeLayers(buf, opts, testIPv4OverGRE...)
		buf.Clear()
	}
}

// testPacketEthernetOverGRE is the packet:
//   11:01:38.124768 IP 192.168.1.1 > 192.168.1.2: GREv0, length 102: IP 172.16.1.1 > 172.16.1.2: ICMP echo request, id 3842, seq 1, length 64
//      0x0000:  ea6b 4cd3 5513 d6b9 d880 56ef 0800 4500  .kL.U.....V...E.
//      0x0010:  007a 0acd 4000 402f ac34 c0a8 0101 c0a8  .z..@.@/.4......
//      0x0020:  0102 0000 6558 aa6a 36e6 c630 6e32 3ec7  ....eX.j6..0n2>.
//      0x0030:  9def 0800 4500 0054 d970 4000 4001 0715  ....E..T.p@.@...
//      0x0040:  ac10 0101 ac10 0102 0800 3f15 0f02 0001  ..........?.....
//      0x0050:  82d9 b154 0000 0000 b5e6 0100 0000 0000  ...T............
//      0x0060:  1011 1213 1415 1617 1819 1a1b 1c1d 1e1f  ................
//      0x0070:  2021 2223 2425 2627 2829 2a2b 2c2d 2e2f  .!"#$%&'()*+,-./
//      0x0080:  3031 3233 3435 3637                      01234567
var testPacketEthernetOverGRE = []byte{
	0xea, 0x6b, 0x4c, 0xd3, 0x55, 0x13, 0xd6, 0xb9, 0xd8, 0x80, 0x56, 0xef, 0x08, 0x00, 0x45, 0x00,
	0x00, 0x7a, 0x0a, 0xcd, 0x40, 0x00, 0x40, 0x2f, 0xac, 0x34, 0xc0, 0xa8, 0x01, 0x01, 0xc0, 0xa8,
	0x01, 0x02, 0x00, 0x00, 0x65, 0x58, 0xaa, 0x6a, 0x36, 0xe6, 0xc6, 0x30, 0x6e, 0x32, 0x3e, 0xc7,
	0x9d, 0xef, 0x08, 0x00, 0x45, 0x00, 0x00, 0x54, 0xd9, 0x70, 0x40, 0x00, 0x40, 0x01, 0x07, 0x15,
	0xac, 0x10, 0x01, 0x01, 0xac, 0x10, 0x01, 0x02, 0x08, 0x00, 0x3f, 0x15, 0x0f, 0x02, 0x00, 0x01,
	0x82, 0xd9, 0xb1, 0x54, 0x00, 0x00, 0x00, 0x00, 0xb5, 0xe6, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
}

func TestPacketEthernetOverGRE(t *testing.T) {
	p := gopacket.NewPacket(testPacketEthernetOverGRE, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeGRE, LayerTypeEthernet, LayerTypeIPv4, LayerTypeICMPv4, gopacket.LayerTypePayload}, t)
	if got, ok := p.Layer(LayerTypeGRE).(*GRE); ok {
		want := &GRE{
			BaseLayer: BaseLayer{testPacketEthernetOverGRE[34:38], testPacketEthernetOverGRE[38:]},
			Protocol:  EthernetTypeTransparentEthernetBridging,
		}
		if !reflect.DeepEqual(want, got) {
			t.Errorf("GRE layer mismatch, \nwant %#v\ngot  %#v\n", want, got)
		}
	}
}

func BenchmarkDecodePacketEthernetOverGRE(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(testPacketEthernetOverGRE, LinkTypeEthernet, gopacket.NoCopy)
	}
}

var testEthernetOverGRE = []gopacket.SerializableLayer{
	&Ethernet{
		SrcMAC:       net.HardwareAddr{0xd6, 0xb9, 0xd8, 0x80, 0x56, 0xef},
		DstMAC:       net.HardwareAddr{0xea, 0x6b, 0x4c, 0xd3, 0x55, 0x13},
		EthernetType: EthernetTypeIPv4,
	},
	&IPv4{
		Version:  4,
		SrcIP:    net.IP{192, 168, 1, 1},
		DstIP:    net.IP{192, 168, 1, 2},
		Protocol: IPProtocolGRE,
		Flags:    IPv4DontFragment,
		TTL:      64,
		Id:       2765,
		IHL:      5,
	},
	&GRE{
		Protocol: EthernetTypeTransparentEthernetBridging,
	},
	&Ethernet{
		SrcMAC:       net.HardwareAddr{0x6e, 0x32, 0x3e, 0xc7, 0x9d, 0xef},
		DstMAC:       net.HardwareAddr{0xaa, 0x6a, 0x36, 0xe6, 0xc6, 0x30},
		EthernetType: EthernetTypeIPv4,
	},
	&IPv4{
		Version:  4,
		SrcIP:    net.IP{172, 16, 1, 1},
		DstIP:    net.IP{172, 16, 1, 2},
		Protocol: IPProtocolICMPv4,
		Flags:    IPv4DontFragment,
		TTL:      64,
		IHL:      5,
		Id:       55664,
	},
	&ICMPv4{
		TypeCode: CreateICMPv4TypeCode(ICMPv4TypeEchoRequest, 0),
		Id:       3842,
		Seq:      1,
	},
	gopacket.Payload{
		0x82, 0xd9, 0xb1, 0x54, 0x00, 0x00, 0x00, 0x00, 0xb5, 0xe6, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
		0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
		0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	},
}

func TestEthernetOverGREEncode(t *testing.T) {
	b := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{
		ComputeChecksums: true,
		FixLengths:       true,
	}
	if err := gopacket.SerializeLayers(b, opts, testEthernetOverGRE...); err != nil {
		t.Errorf("Unable to serialize: %v", err)
	}
	p := gopacket.NewPacket(b.Bytes(), LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeGRE, LayerTypeEthernet, LayerTypeIPv4, LayerTypeICMPv4, gopacket.LayerTypePayload}, t)
	if got, want := b.Bytes(), testPacketEthernetOverGRE; !reflect.DeepEqual(want, got) {
		t.Errorf("Encoding mismatch, \nwant: %v\ngot %v\n", want, got)
	}
}

func BenchmarkEncodePacketEthernetOverGRE(b *testing.B) {
	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{
		ComputeChecksums: true,
		FixLengths:       true,
	}
	for i := 0; i < b.N; i++ {
		gopacket.SerializeLayers(buf, opts, testEthernetOverGRE...)
		buf.Clear()
	}
}

var testGREChecksum = map[uint16][]gopacket.SerializableLayer{
	0x77ff: {
		&Ethernet{
			SrcMAC:       net.HardwareAddr{0xc2, 0x00, 0x57, 0x75, 0x00, 0x00},
			DstMAC:       net.HardwareAddr{0xc2, 0x01, 0x57, 0x75, 0x00, 0x00},
			EthernetType: EthernetTypeIPv4,
		},
		&IPv4{
			Version:  4,
			SrcIP:    net.IP{10, 0, 0, 1},
			DstIP:    net.IP{10, 0, 0, 2},
			Protocol: IPProtocolGRE,
			TTL:      255,
			Id:       10,
			IHL:      5,
		},
		&GRE{
			Protocol:        EthernetTypeIPv4,
			ChecksumPresent: true,
		},
		&IPv4{
			Version:  4,
			SrcIP:    net.IP{1, 1, 1, 1},
			DstIP:    net.IP{2, 2, 2, 2},
			Protocol: IPProtocolICMPv4,
			TTL:      255,
			IHL:      5,
			Id:       10,
		},
		&ICMPv4{
			TypeCode: CreateICMPv4TypeCode(ICMPv4TypeEchoRequest, 0),
			Id:       2,
			Seq:      0,
		},
		gopacket.Payload{
			0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xbe, 0x70, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
			0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
			0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
			0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
			0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd, 0xab, 0xcd,
		},
	},
	0x8475: {
		&Ethernet{
			SrcMAC:       net.HardwareAddr{0xc2, 0x00, 0x57, 0x75, 0x00, 0x00},
			DstMAC:       net.HardwareAddr{0xc2, 0x01, 0x57, 0x75, 0x00, 0x00},
			EthernetType: EthernetTypeIPv4,
		},
		&IPv4{
			Version:  4,
			SrcIP:    net.IP{10, 0, 0, 1},
			DstIP:    net.IP{10, 0, 0, 2},
			Protocol: IPProtocolGRE,
			TTL:      255,
			Id:       10,
			IHL:      5,
		},
		&GRE{
			Protocol:        EthernetTypeIPv4,
			ChecksumPresent: true,
		},
		&IPv4{
			Version:  4,
			SrcIP:    net.IP{2, 3, 4, 5},
			DstIP:    net.IP{2, 3, 4, 50},
			Protocol: IPProtocolUDP,
			TTL:      1,
			IHL:      5,
			Flags:    IPv4DontFragment,
			Id:       964,
		},
		&UDP{
			SrcPort: 41781,
			DstPort: 33434,
		},
		gopacket.Payload{
			0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f,
			0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
		},
	},
}

func TestGREChecksum(t *testing.T) {
	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{
		ComputeChecksums: true,
		FixLengths:       true,
	}
	for cksum, packet := range testGREChecksum {
		buf.Clear()
		if err := setNetworkLayer(packet); err != nil {
			t.Errorf("Failed to set network layer: %v", err)
			continue
		}
		if err := gopacket.SerializeLayers(buf, opts, packet...); err != nil {
			t.Errorf("Failed to serialize packet: %v", err)
			continue
		}
		p := gopacket.NewPacket(buf.Bytes(), LinkTypeEthernet, gopacket.Default)
		t.Log(p)
		if p.ErrorLayer() != nil {
			t.Error("Failed to decode packet:", p.ErrorLayer().Error())
			continue
		}
		if got, ok := p.Layer(LayerTypeGRE).(*GRE); ok {
			if got.Checksum != cksum {
				t.Errorf("Incorrect checksum calculated for GRE packet: want %v, got %v", cksum, got.Checksum)
			}
		}
	}
}

func setNetworkLayer(layers []gopacket.SerializableLayer) error {
	type setNetworkLayerForChecksum interface {
		SetNetworkLayerForChecksum(gopacket.NetworkLayer) error
	}
	var l gopacket.NetworkLayer
	for _, layer := range layers {
		if n, ok := layer.(gopacket.NetworkLayer); ok {
			l = n
		}
		if s, ok := layer.(setNetworkLayerForChecksum); ok {
			if l == nil {
				return fmt.Errorf("no enclosing network layer found before: %v", s)
			}
			if err := s.SetNetworkLayerForChecksum(l); err != nil {
				return fmt.Errorf("failed to set network layer(%v) on layer(%v): %v", l, s, err)
			}
		}
	}
	return nil
}
