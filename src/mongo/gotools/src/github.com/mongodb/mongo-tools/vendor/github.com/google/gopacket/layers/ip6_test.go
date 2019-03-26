// Copyright 2014, Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"bytes"
	"github.com/google/gopacket"
	"net"
	"reflect"
	"testing"
)

func TestSerializeIPv6HeaderTLVOptions(t *testing.T) {
	//RFC 2460 Appendix B
	/*
	   Example 3

	   A Hop-by-Hop or Destination Options header containing both options X
	   and Y from Examples 1 and 2 would have one of the two following
	   formats, depending on which option appeared first:

	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |  Next Header  | Hdr Ext Len=3 | Option Type=X |Opt Data Len=12|
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |                         4-octet field                         |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |                                                               |
	   +                         8-octet field                         +
	   |                                                               |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   | PadN Option=1 |Opt Data Len=1 |       0       | Option Type=Y |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |Opt Data Len=7 | 1-octet field |         2-octet field         |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |                         4-octet field                         |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   | PadN Option=1 |Opt Data Len=2 |       0       |       0       |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	*/
	opt1 := &ipv6HeaderTLVOption{}
	opt1.OptionType = 0x1e
	opt1.OptionData = []byte{0xaa, 0xaa, 0xaa, 0xaa, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb}
	opt1.OptionAlignment = [2]uint8{8, 2}

	opt2 := &ipv6HeaderTLVOption{}
	opt2.OptionType = 0x3e
	opt2.OptionData = []byte{0x11, 0x22, 0x22, 0x44, 0x44, 0x44, 0x44}
	opt2.OptionAlignment = [2]uint8{4, 3}

	l := serializeIPv6HeaderTLVOptions(nil, []*ipv6HeaderTLVOption{opt1, opt2}, true)
	b := make([]byte, l)
	serializeIPv6HeaderTLVOptions(b, []*ipv6HeaderTLVOption{opt1, opt2}, true)
	got := b
	want := []byte{0x1e, 0x0c, 0xaa, 0xaa, 0xaa, 0xaa, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0x01, 0x01, 0x00, 0x3e, 0x07, 0x11, 0x22, 0x22, 0x44, 0x44, 0x44, 0x44, 0x01, 0x02, 0x00, 0x00}

	if !bytes.Equal(got, want) {
		t.Errorf("IPv6HeaderTLVOption serialize (X,Y) failed:\ngot:\n%#v\n\nwant:\n%#v\n\n", got, want)
	}

	/*
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |  Next Header  | Hdr Ext Len=3 | Pad1 Option=0 | Option Type=Y |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |Opt Data Len=7 | 1-octet field |         2-octet field         |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |                         4-octet field                         |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   | PadN Option=1 |Opt Data Len=4 |       0       |       0       |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |       0       |       0       | Option Type=X |Opt Data Len=12|
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |                         4-octet field                         |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	   |                                                               |
	   +                         8-octet field                         +
	   |                                                               |
	   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
	*/

	l = serializeIPv6HeaderTLVOptions(nil, []*ipv6HeaderTLVOption{opt2, opt1}, true)
	b = make([]byte, l)
	serializeIPv6HeaderTLVOptions(b, []*ipv6HeaderTLVOption{opt2, opt1}, true)
	got = b
	want = []byte{0x00, 0x3e, 0x07, 0x11, 0x22, 0x22, 0x44, 0x44, 0x44, 0x44, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x0c, 0xaa, 0xaa, 0xaa, 0xaa, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb, 0xbb}

	if !bytes.Equal(got, want) {
		t.Errorf("IPv6HeaderTLVOption serialize (Y,X) failed:\ngot:\n%#v\n\nwant:\n%#v\n\n", got, want)
	}
}

var testPacketIPv6HopByHop0 = []byte{
	0x60, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x40, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x3b, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00,
}

func TestPacketIPv6HopByHop0Serialize(t *testing.T) {
	var serialize = make([]gopacket.SerializableLayer, 0, 2)
	var err error

	ip6 := &IPv6{}
	ip6.Version = 6
	ip6.NextHeader = IPProtocolIPv6HopByHop
	ip6.HopLimit = 64
	ip6.SrcIP = net.ParseIP("2001:db8::1")
	ip6.DstIP = net.ParseIP("2001:db8::2")
	serialize = append(serialize, ip6)

	tlv := &IPv6HopByHopOption{}
	tlv.OptionType = 0x01 //PadN
	tlv.OptionData = []byte{0x00, 0x00, 0x00, 0x00}
	hop := &IPv6HopByHop{}
	hop.Options = append(hop.Options, tlv)
	hop.NextHeader = IPProtocolNoNextHeader
	ip6.HopByHop = hop

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true}
	err = gopacket.SerializeLayers(buf, opts, serialize...)
	if err != nil {
		t.Fatal(err)
	}

	got := buf.Bytes()
	want := testPacketIPv6HopByHop0
	if !bytes.Equal(got, want) {
		t.Errorf("IPv6HopByHop serialize failed:\ngot:\n%#v\n\nwant:\n%#v\n\n", got, want)
	}
}

func TestPacketIPv6HopByHop0Decode(t *testing.T) {
	ip6 := &IPv6{
		BaseLayer: BaseLayer{
			Contents: []byte{
				0x60, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x40, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
			},
			Payload: []byte{},
		},
		Version:      6,
		TrafficClass: 0,
		FlowLabel:    0,
		Length:       8,
		NextHeader:   IPProtocolIPv6HopByHop,
		HopLimit:     64,
		SrcIP:        net.IP{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01},
		DstIP: net.IP{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02},
	}

	hop := &ip6.hbh
	hop.ipv6ExtensionBase = ipv6ExtensionBase{
		BaseLayer: BaseLayer{
			Contents: []byte{0x3b, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00},
			Payload:  []byte{},
		},
		NextHeader:   IPProtocolNoNextHeader,
		HeaderLength: uint8(0),
		ActualLength: 8,
	}
	opt := &IPv6HopByHopOption{
		OptionType:   uint8(0x01),
		OptionLength: uint8(0x04),
		ActualLength: 6,
		OptionData:   []byte{0x00, 0x00, 0x00, 0x00},
	}
	hop.Options = append(hop.Options, opt)
	ip6.HopByHop = hop

	p := gopacket.NewPacket(testPacketIPv6HopByHop0, LinkTypeRaw, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeIPv6, LayerTypeIPv6HopByHop}, t)
	if got, ok := p.Layer(LayerTypeIPv6).(*IPv6); ok {
		want := ip6
		want.HopByHop = got.HopByHop // avoid comparing pointers
		if !reflect.DeepEqual(got, want) {
			t.Errorf("IPv6 packet processing failed:\ngot:\n%#v\n\nwant:\n%#v\n\n", got, want)
		}
	} else {
		t.Error("No IPv6 layer type found in packet")
	}
	if got, ok := p.Layer(LayerTypeIPv6HopByHop).(*IPv6HopByHop); ok {
		want := hop
		if !reflect.DeepEqual(got, want) {
			t.Errorf("IPv6HopByHop packet processing failed:\ngot\n%#v\n\nwant:\n%#v\n\n", got, want)
		}
	} else {
		t.Error("No IPv6HopByHop layer type found in packet")
	}
}

// testPacketIPv6Destination0 is the packet:
//   12:40:14.429409595 IP6 2001:db8::1 > 2001:db8::2: DSTOPT no next header
//   	0x0000:  6000 0000 0008 3c40 2001 0db8 0000 0000  `.....<@........
//   	0x0010:  0000 0000 0000 0001 2001 0db8 0000 0000  ................
//   	0x0020:  0000 0000 0000 0002 3b00 0104 0000 0000  ........;.......
var testPacketIPv6Destination0 = []byte{
	0x60, 0x00, 0x00, 0x00, 0x00, 0x08, 0x3c, 0x40, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x3b, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00,
}

func TestPacketIPv6Destination0Serialize(t *testing.T) {
	var serialize = make([]gopacket.SerializableLayer, 0, 2)
	var err error

	ip6 := &IPv6{}
	ip6.Version = 6
	ip6.NextHeader = IPProtocolIPv6Destination
	ip6.HopLimit = 64
	ip6.SrcIP = net.ParseIP("2001:db8::1")
	ip6.DstIP = net.ParseIP("2001:db8::2")
	serialize = append(serialize, ip6)

	tlv := &IPv6DestinationOption{}
	tlv.OptionType = 0x01 //PadN
	tlv.OptionData = []byte{0x00, 0x00, 0x00, 0x00}
	dst := &IPv6Destination{}
	dst.Options = append(dst.Options, tlv)
	dst.NextHeader = IPProtocolNoNextHeader
	serialize = append(serialize, dst)

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true}
	err = gopacket.SerializeLayers(buf, opts, serialize...)
	if err != nil {
		t.Fatal(err)
	}

	got := buf.Bytes()
	want := testPacketIPv6Destination0
	if !bytes.Equal(got, want) {
		t.Errorf("IPv6Destination serialize failed:\ngot:\n%#v\n\nwant:\n%#v\n\n", got, want)
	}
}

func TestPacketIPv6Destination0Decode(t *testing.T) {
	ip6 := &IPv6{
		BaseLayer: BaseLayer{
			Contents: []byte{
				0x60, 0x00, 0x00, 0x00, 0x00, 0x08, 0x3c, 0x40, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
			},
			Payload: []byte{0x3b, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00},
		},
		Version:      6,
		TrafficClass: 0,
		FlowLabel:    0,
		Length:       8,
		NextHeader:   IPProtocolIPv6Destination,
		HopLimit:     64,
		SrcIP:        net.IP{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01},
		DstIP: net.IP{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02},
	}

	dst := &IPv6Destination{}
	dst.BaseLayer = BaseLayer{
		Contents: []byte{0x3b, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00},
		Payload:  []byte{},
	}
	dst.NextHeader = IPProtocolNoNextHeader
	dst.HeaderLength = uint8(0)
	dst.ActualLength = 8
	opt := &IPv6DestinationOption{
		OptionType:   uint8(0x01),
		OptionLength: uint8(0x04),
		ActualLength: 6,
		OptionData:   []byte{0x00, 0x00, 0x00, 0x00},
	}
	dst.Options = append(dst.Options, opt)

	p := gopacket.NewPacket(testPacketIPv6Destination0, LinkTypeRaw, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeIPv6, LayerTypeIPv6Destination}, t)
	if got, ok := p.Layer(LayerTypeIPv6).(*IPv6); ok {
		want := ip6
		if !reflect.DeepEqual(got, want) {
			t.Errorf("IPv6 packet processing failed:\ngot:\n%#v\n\nwant:\n%#v\n\n", got, want)
		}
	} else {
		t.Error("No IPv6 layer type found in packet")
	}
	if got, ok := p.Layer(LayerTypeIPv6Destination).(*IPv6Destination); ok {
		want := dst
		if !reflect.DeepEqual(got, want) {
			t.Errorf("IPv6Destination packet processing failed:\ngot:\n%#v\n\nwant:\n%#v\n\n", got, want)
		}
	} else {
		t.Error("No IPv6Destination layer type found in packet")
	}
}

var testPacketIPv6JumbogramHeader = []byte{
	0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x3b, 0x00, 0xc2, 0x04, 0x00, 0x01, 0x00, 0x08,
}

func TestIPv6JumbogramSerialize(t *testing.T) {
	var serialize = make([]gopacket.SerializableLayer, 0, 2)
	var err error

	ip6 := &IPv6{}
	ip6.Version = 6
	ip6.NextHeader = IPProtocolNoNextHeader
	ip6.HopLimit = 64
	ip6.SrcIP = net.ParseIP("2001:db8::1")
	ip6.DstIP = net.ParseIP("2001:db8::2")
	serialize = append(serialize, ip6)

	payload := make([]byte, ipv6MaxPayloadLength+1)
	for i := range payload {
		payload[i] = 0xfe
	}
	serialize = append(serialize, gopacket.Payload(payload))

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true}
	err = gopacket.SerializeLayers(buf, opts, serialize...)
	if err != nil {
		t.Fatal(err)
	}

	got := buf.Bytes()
	w := new(bytes.Buffer)
	w.Write(testPacketIPv6JumbogramHeader)
	w.Write(payload)
	want := w.Bytes()

	if !bytes.Equal(got, want) {
		t.Errorf("IPv6 Jumbogram serialize failed:\ngot:\n%v\n\nwant:\n%v\n\n",
			gopacket.LongBytesGoString(got), gopacket.LongBytesGoString(want))
	}
}

func TestIPv6JumbogramDecode(t *testing.T) {
	payload := make([]byte, ipv6MaxPayloadLength+1)
	for i := range payload {
		payload[i] = 0xfe
	}

	ip6 := &IPv6{
		BaseLayer: BaseLayer{
			Contents: []byte{
				0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
			},
		},
		Version:      6,
		TrafficClass: 0,
		FlowLabel:    0,
		Length:       0,
		NextHeader:   IPProtocolIPv6HopByHop,
		HopLimit:     64,
		SrcIP:        net.IP{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01},
		DstIP: net.IP{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02},
	}
	buf := new(bytes.Buffer)
	buf.Write([]byte{0x3b, 0x00, 0xc2, 0x04, 0x00, 0x01, 0x00, 0x08})
	buf.Write(payload)
	ip6.Payload = buf.Bytes()

	hop := &ip6.hbh
	hop.Contents = []byte{0x3b, 0x00, 0xc2, 0x04, 0x00, 0x01, 0x00, 0x08}
	hop.Payload = payload
	hop.NextHeader = IPProtocolNoNextHeader
	hop.HeaderLength = uint8(0)
	hop.ActualLength = 8
	opt := &IPv6HopByHopOption{}
	opt.OptionType = uint8(0xc2)
	opt.OptionLength = uint8(0x04)
	opt.ActualLength = 6
	opt.OptionData = []byte{0x00, 0x01, 0x00, 0x08}
	hop.Options = append(hop.Options, opt)
	ip6.HopByHop = hop

	pkt := new(bytes.Buffer)
	pkt.Write(testPacketIPv6JumbogramHeader)
	pkt.Write(payload)

	p := gopacket.NewPacket(pkt.Bytes(), LinkTypeRaw, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeIPv6, LayerTypeIPv6HopByHop, gopacket.LayerTypePayload}, t)

	if got, ok := p.Layer(LayerTypeIPv6).(*IPv6); ok {
		want := ip6
		want.HopByHop = got.HopByHop // Hack, avoid comparing pointers
		if !reflect.DeepEqual(got, want) {
			t.Errorf("IPv6 packet processing failed:\ngot:\n%v\n\nwant:\n%v\n\n",
				gopacket.LayerGoString(got), gopacket.LayerGoString(want))
		}
	} else {
		t.Error("No IPv6 layer type found in packet")
	}

	if got, ok := p.Layer(LayerTypeIPv6HopByHop).(*IPv6HopByHop); ok {
		want := hop
		if !reflect.DeepEqual(got, want) {
			t.Errorf("IPv6HopByHop packet processing failed:\ngot:\n%v\n\nwant:\n%v\n\n",
				gopacket.LayerGoString(got), gopacket.LayerGoString(want))
		}
	} else {
		t.Error("No IPv6HopByHop layer type found in packet")
	}

	if got, ok := p.Layer(gopacket.LayerTypePayload).(*gopacket.Payload); ok {
		want := (*gopacket.Payload)(&payload)
		if !reflect.DeepEqual(got, want) {
			t.Errorf("Payload packet processing failed:\ngot:\n%v\n\nwant:\n%v\n\n",
				gopacket.LayerGoString(got), gopacket.LayerGoString(want))
		}
	} else {
		t.Error("No Payload layer type found in packet")
	}
}
