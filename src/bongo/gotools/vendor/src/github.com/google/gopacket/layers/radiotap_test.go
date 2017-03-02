// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.
package layers

import (
	"github.com/google/gopacket"
	"testing"
)

// testPacketRadiotap0 is the packet:
//   09:34:34.799438 1.0 Mb/s 2412 MHz 11b -58dB signal antenna 7 Acknowledgment RA:88:1f:a1:ae:9d:cb
//      0x0000:  0000 1200 2e48 0000 1002 6c09 a000 c607  .....H....l.....
//      0x0010:  0000 d400 0000 881f a1ae 9dcb c630 4b4b  .............0KK
var testPacketRadiotap0 = []byte{
	0x00, 0x00, 0x12, 0x00, 0x2e, 0x48, 0x00, 0x00, 0x10, 0x02, 0x6c, 0x09, 0xa0, 0x00, 0xc6, 0x07,
	0x00, 0x00, 0xd4, 0x00, 0x00, 0x00, 0x88, 0x1f, 0xa1, 0xae, 0x9d, 0xcb, 0xc6, 0x30, 0x4b, 0x4b,
}

func TestPacketRadiotap0(t *testing.T) {
	p := gopacket.NewPacket(testPacketRadiotap0, LayerTypeRadioTap, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeRadioTap, LayerTypeDot11}, t)
	rt := p.Layer(LayerTypeRadioTap).(*RadioTap)
	if rt.ChannelFrequency != 2412 || rt.DBMAntennaSignal != -58 || rt.Antenna != 7 {
		t.Error("Radiotap decode error")
	}
	if rt.Rate != 2 { // 500Kbps unit
		t.Error("Radiotap Rate decode error")
	}
}
func BenchmarkDecodePacketRadiotap0(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(testPacketRadiotap0, LayerTypeRadioTap, gopacket.NoCopy)
	}
}

// testPacketRadiotap1 is the packet:
//   05:24:21.380948 2412 MHz 11g -36dB signal antenna 5 65.0 Mb/s MCS 7 20 MHz lon GI
//   	0x0000:  0000 1500 2a48 0800 1000 6c09 8004 dc05  ....*H....l.....
//   	0x0010:  0000 0700 0748 112c 0000 3a9d aaf0 191c  .....H.,..:.....
//   	0x0020:  aba7 f213 9d00 3a9d aaf0 1970 b2ee a9f1  ......:....p....
//   	0x0030:  16                                       .
var testPacketRadiotap1 = []byte{
	0x00, 0x00, 0x15, 0x00, 0x2a, 0x48, 0x08, 0x00, 0x10, 0x00, 0x6c, 0x09, 0x80, 0x04, 0xdc, 0x05,
	0x00, 0x00, 0x07, 0x00, 0x07, 0x48, 0x11, 0x2c, 0x00, 0x00, 0x3a, 0x9d, 0xaa, 0xf0, 0x19, 0x1c,
	0xab, 0xa7, 0xf2, 0x13, 0x9d, 0x00, 0x3a, 0x9d, 0xaa, 0xf0, 0x19, 0x70, 0xb2, 0xee, 0xa9, 0xf1,
	0x16,
}

func TestPacketRadiotap1(t *testing.T) {
	p := gopacket.NewPacket(testPacketRadiotap1, LayerTypeRadioTap, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeRadioTap, LayerTypeDot11}, t)
	rt := p.Layer(LayerTypeRadioTap).(*RadioTap)
	if rt.ChannelFrequency != 2412 || rt.DBMAntennaSignal != -36 || rt.Antenna != 5 {
		t.Error("Radiotap decode error")
	}
	if !rt.MCS.Known.MCSIndex() || rt.MCS.MCS != 7 {
		t.Error("Radiotap MCS error")
	}
	if !rt.MCS.Known.Bandwidth() || rt.MCS.Flags.Bandwidth() != 0 {
		t.Error("Radiotap bandwidth error")
	}
	if !rt.MCS.Known.GuardInterval() || rt.MCS.Flags.ShortGI() {
		t.Error("Radiotap GI error")
	}
}
func BenchmarkDecodePacketRadiotap1(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(testPacketRadiotap1, LayerTypeRadioTap, gopacket.NoCopy)
	}
}
