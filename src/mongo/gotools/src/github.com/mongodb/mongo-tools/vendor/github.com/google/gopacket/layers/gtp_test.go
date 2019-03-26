// Copyright 2017 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.
//

package layers

import (
	"github.com/google/gopacket"
	"reflect"
	"testing"
)

// testGTPPacket is the packet:
//0000  00 0c 29 e3 c6 4d 00 0c  29 da d1 de 08 00 45 00   ..)..M.. ).....E.
//0010  00 7c 00 00 40 00 40 11  67 bb c0 a8 28 b2 c0 a8   .|..@.@. g...(...
//0020  28 b3 08 68 08 68 00 68  c1 c4 32 ff 00 58 00 00   (..h.h.h ..2..X..
//0030  00 01 26 7b 00 00 45 00  00 54 06 76 00 00 40 01   ..&{..E. .T.v..@.
//0040  98 2f c0 a8 28 b2 ca 0b  28 9e 00 00 39 e9 00 00   ./..(... (...9...
//0050  28 7d 06 11 20 4b 7f 3a  0d 00 08 09 0a 0b 0c 0d   (}.. K.: ........
//0060  0e 0f 10 11 12 13 14 15  16 17 18 19 1a 1b 1c 1d   ........ ........
//0070  1e 1f 20 21 22 23 24 25  26 27 28 29 2a 2b 2c 2d   .. !"#$% &'()*+,-
//0080  2e 2f 30 31 32 33 34 35  36 37                     ./012345 67

var testGTPPacket = []byte{
	0x00, 0x0c, 0x29, 0xe3, 0xc6, 0x4d, 0x00, 0x0c,
	0x29, 0xda, 0xd1, 0xde, 0x08, 0x00, 0x45, 0x00,
	0x00, 0x7c, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
	0x67, 0xbb, 0xc0, 0xa8, 0x28, 0xb2, 0xc0, 0xa8,
	0x28, 0xb3, 0x08, 0x68, 0x08, 0x68, 0x00, 0x68,
	0xc1, 0xc4, 0x32, 0xff, 0x00, 0x58, 0x00, 0x00,
	0x00, 0x01, 0x26, 0x7b, 0x00, 0x00, 0x45, 0x00,
	0x00, 0x54, 0x06, 0x76, 0x00, 0x00, 0x40, 0x01,
	0x98, 0x2f, 0xc0, 0xa8, 0x28, 0xb2, 0xca, 0x0b,
	0x28, 0x9e, 0x00, 0x00, 0x39, 0xe9, 0x00, 0x00,
	0x28, 0x7d, 0x06, 0x11, 0x20, 0x4b, 0x7f, 0x3a,
	0x0d, 0x00, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
	0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d,
	0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
	0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,
	0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
	0x36, 0x37,
}

func TestGTPPacket(t *testing.T) {
	p := gopacket.NewPacket(testGTPPacket, LayerTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeUDP, LayerTypeGTPv1U, LayerTypeIPv4,
		LayerTypeICMPv4, gopacket.LayerTypePayload}, t)
	if got, ok := p.Layer(LayerTypeGTPv1U).(*GTPv1U); ok {
		want := &GTPv1U{
			Version:             1,
			ProtocolType:        1,
			Reserved:            0,
			ExtensionHeaderFlag: false,
			SequenceNumberFlag:  true,
			NPDUFlag:            false,
			MessageType:         255,
			MessageLength:       88,
			TEID:                1,
			SequenceNumber:      9851,
		}
		want.BaseLayer = BaseLayer{testGTPPacket[42:54], testGTPPacket[54:]}
		if !reflect.DeepEqual(got, want) {
			t.Errorf("GTP packet mismatch:\ngot  :\n%#v\n\nwant :\n%#v\n\n", got, want)

		}
		buf := gopacket.NewSerializeBuffer()
		opts := gopacket.SerializeOptions{}
		err := got.SerializeTo(buf, opts)
		if err != nil {
			t.Error(err)
		}
		if !reflect.DeepEqual(got.Contents, buf.Bytes()) {
			t.Errorf("GTP packet serialization failed:\ngot  :\n%#v\n\nwant :\n%#v\n\n", buf.Bytes(), got.Contents)
		}
	} else {
		t.Error("Incorrect gtp packet")
	}
}

// testGTPPacketWithEH is the packet
//000000 00 0c 29 e3 c6 4d 00 0c 29 da d1 de 08 00 45 00 ..)..M..).....E.
//000010 00 80 00 00 40 00 40 11 67 bb c0 a8 28 b2 c0 a8 ....@.@.g...(...
//000020 28 b3 08 68 08 68 00 6c c1 95 36 ff 00 58 00 10 (..h.h.l..6..X..
//000030 06 57 00 05 00 c0 01 09 04 00 45 00 00 54 06 a5 .W........E..T..
//000040 00 00 40 01 98 00 c0 a8 28 b2 ca 0b 28 9e 00 00 ..@.....(...(...
//000050 e3 b6 00 00 28 ac 35 11 20 4b a6 3d 0d 00 08 09 ....(.5. K.=....
//000060 0a 0b 0c 0d 0e 0f 10 11 12 13 14 15 16 17 18 19 ................
//000070 1a 1b 1c 1d 1e 1f 20 21 22 23 24 25 26 27 28 29 ...... !"#$%&'()
//000080 2a 2b 2c 2d 2e 2f 30 31 32 33 34 35 36 37

var testGTPPacketWithEH = []byte{
	0x00, 0x0c, 0x29, 0xe3, 0xc6, 0x4d, 0x00, 0x0c,
	0x29, 0xda, 0xd1, 0xde, 0x08, 0x00, 0x45, 0x00,
	0x00, 0x80, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
	0x67, 0xbb, 0xc0, 0xa8, 0x28, 0xb2, 0xc0, 0xa8,
	0x28, 0xb3, 0x08, 0x68, 0x08, 0x68, 0x00, 0x6c,
	0xc1, 0x95, 0x36, 0xff, 0x00, 0x58, 0x00, 0x10,
	0x06, 0x57, 0x00, 0x05, 0x00, 0xc0, 0x01, 0x09,
	0x04, 0x00, 0x45, 0x00, 0x00, 0x54, 0x06, 0xa5,
	0x00, 0x00, 0x40, 0x01, 0x98, 0x00, 0xc0, 0xa8,
	0x28, 0xb2, 0xca, 0x0b, 0x28, 0x9e, 0x00, 0x00,
	0xe3, 0xb6, 0x00, 0x00, 0x28, 0xac, 0x35, 0x11,
	0x20, 0x4b, 0xa6, 0x3d, 0x0d, 0x00, 0x08, 0x09,
	0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
	0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
	0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21,
	0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
	0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31,
	0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
}

func TestGTPPacketWithEH(t *testing.T) {
	p := gopacket.NewPacket(testGTPPacketWithEH, LayerTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEthernet, LayerTypeIPv4, LayerTypeUDP, LayerTypeGTPv1U, LayerTypeIPv4,
		LayerTypeICMPv4, gopacket.LayerTypePayload}, t)
	if got, ok := p.Layer(LayerTypeGTPv1U).(*GTPv1U); ok {
		want := &GTPv1U{
			Version:             1,
			ProtocolType:        1,
			Reserved:            0,
			ExtensionHeaderFlag: true,
			SequenceNumberFlag:  true,
			NPDUFlag:            false,
			MessageType:         255,
			MessageLength:       88,
			TEID:                1050199,
			SequenceNumber:      5,
			GTPExtensionHeaders: []GTPExtensionHeader{GTPExtensionHeader{Type: uint8(192), Content: []byte{0x9, 0x4}}},
		}
		want.BaseLayer = BaseLayer{testGTPPacketWithEH[42:58], testGTPPacketWithEH[58:]}
		if !reflect.DeepEqual(got, want) {
			t.Errorf("GTP packet mismatch:\ngot  :\n%#v\n\nwant :\n%#v\n\n", got, want)

		}
		buf := gopacket.NewSerializeBuffer()
		opts := gopacket.SerializeOptions{}
		err := got.SerializeTo(buf, opts)
		if err != nil {
			t.Error(err)
		}
		if !reflect.DeepEqual(got.Contents, buf.Bytes()) {
			t.Errorf("GTP packet serialization failed:\ngot  :\n%#v\n\nbuf :\n%#v\n\n", got.Contents, buf.Bytes())
		}
	} else {
		t.Errorf("Invalid GTP packet")
	}

}
