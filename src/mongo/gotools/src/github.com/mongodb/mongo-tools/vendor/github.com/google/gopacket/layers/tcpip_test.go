// Copyright 2014, Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"github.com/google/gopacket"
	"net"
	"testing"
)

const (
	ipv4UDPChecksum                = uint16(0xbc5f) // Wireshark
	ipv6UDPChecksumWithIPv6DstOpts = uint16(0x4d21) // Wireshark
	ipv6UDPChecksumJumbogram       = uint16(0xcda8)
)

func createIPv4ChecksumTestLayer() (ip4 *IPv4) {
	ip4 = &IPv4{}
	ip4.Version = 4
	ip4.TTL = 64
	ip4.SrcIP = net.ParseIP("192.0.2.1")
	ip4.DstIP = net.ParseIP("198.51.100.1")
	return
}

func createIPv6ChecksumTestLayer() (ip6 *IPv6) {
	ip6 = &IPv6{}
	ip6.Version = 6
	ip6.NextHeader = IPProtocolNoNextHeader
	ip6.HopLimit = 64
	ip6.SrcIP = net.ParseIP("2001:db8::1")
	ip6.DstIP = net.ParseIP("2001:db8::2")
	return
}

func createIPv6DestinationChecksumTestLayer() (dst *IPv6Destination) {
	tlv := &IPv6DestinationOption{}
	tlv.OptionType = 0x01 //PadN
	tlv.OptionData = []byte{0x00, 0x00, 0x00, 0x00}
	dst = &IPv6Destination{}
	dst.Options = append(dst.Options, tlv)
	dst.NextHeader = IPProtocolNoNextHeader
	return
}

func createUDPChecksumTestLayer() (udp *UDP) {
	udp = &UDP{}
	udp.SrcPort = UDPPort(12345)
	udp.DstPort = UDPPort(9999)
	return
}

func TestIPv4UDPChecksum(t *testing.T) {
	var serialize = make([]gopacket.SerializableLayer, 0, 2)
	var u *UDP
	var err error

	ip4 := createIPv4ChecksumTestLayer()
	ip4.Protocol = IPProtocolUDP
	serialize = append(serialize, ip4)

	udp := createUDPChecksumTestLayer()
	udp.SetNetworkLayerForChecksum(ip4)
	serialize = append(serialize, udp)

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true, ComputeChecksums: true}
	err = gopacket.SerializeLayers(buf, opts, serialize...)
	if err != nil {
		t.Fatal(err)
	}

	p := gopacket.NewPacket(buf.Bytes(), LinkTypeRaw, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Fatal("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeIPv4, LayerTypeUDP}, t)

	if l, ok := p.Layer(LayerTypeUDP).(*UDP); !ok {
		t.Fatal("No UDP layer type found in packet")
	} else {
		u = l
	}
	got := u.Checksum
	want := ipv4UDPChecksum
	if got != want {
		t.Errorf("Bad checksum:\ngot:\n%#v\n\nwant:\n%#v\n\n", got, want)
	}
}

func TestIPv6UDPChecksumWithIPv6DstOpts(t *testing.T) {
	var serialize = make([]gopacket.SerializableLayer, 0, 3)
	var u *UDP
	var err error

	ip6 := createIPv6ChecksumTestLayer()
	ip6.NextHeader = IPProtocolIPv6Destination
	serialize = append(serialize, ip6)

	dst := createIPv6DestinationChecksumTestLayer()
	dst.NextHeader = IPProtocolUDP
	serialize = append(serialize, dst)

	udp := createUDPChecksumTestLayer()
	udp.SetNetworkLayerForChecksum(ip6)
	serialize = append(serialize, udp)

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true, ComputeChecksums: true}
	err = gopacket.SerializeLayers(buf, opts, serialize...)
	if err != nil {
		t.Fatal(err)
	}

	p := gopacket.NewPacket(buf.Bytes(), LinkTypeRaw, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Fatal("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeIPv6, LayerTypeIPv6Destination, LayerTypeUDP}, t)

	if l, ok := p.Layer(LayerTypeUDP).(*UDP); !ok {
		t.Fatal("No UDP layer type found in packet")
	} else {
		u = l
	}
	got := u.Checksum
	want := ipv6UDPChecksumWithIPv6DstOpts
	if got != want {
		t.Errorf("Bad checksum:\ngot:\n%#v\n\nwant:\n%#v\n\n", got, want)
	}
}

func TestIPv6JumbogramUDPChecksum(t *testing.T) {
	var serialize = make([]gopacket.SerializableLayer, 0, 4)
	var u *UDP
	var err error

	ip6 := &IPv6{}
	ip6.Version = 6
	ip6.NextHeader = IPProtocolUDP
	ip6.HopLimit = 64
	ip6.SrcIP = net.ParseIP("2001:db8::1")
	ip6.DstIP = net.ParseIP("2001:db8::2")
	serialize = append(serialize, ip6)

	udp := &UDP{}
	udp.SrcPort = UDPPort(12345)
	udp.DstPort = UDPPort(9999)
	udp.SetNetworkLayerForChecksum(ip6)
	serialize = append(serialize, udp)

	payload := make([]byte, ipv6MaxPayloadLength+1)
	for i := range payload {
		payload[i] = 0xfe
	}
	serialize = append(serialize, gopacket.Payload(payload))

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true, ComputeChecksums: true}
	err = gopacket.SerializeLayers(buf, opts, serialize...)
	if err != nil {
		t.Fatal(err)
	}

	p := gopacket.NewPacket(buf.Bytes(), LinkTypeRaw, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Fatal("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeIPv6, LayerTypeIPv6HopByHop, LayerTypeUDP, gopacket.LayerTypePayload}, t)

	if l, ok := p.Layer(LayerTypeUDP).(*UDP); !ok {
		t.Fatal("No UDP layer type found in packet")
	} else {
		u = l
	}
	got := u.Checksum
	want := ipv6UDPChecksumJumbogram
	if got != want {
		t.Errorf("Bad checksum:\ngot:\n%#v\n\nwant:\n%#v\n\n", got, want)
	}
}
