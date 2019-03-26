// Copyright placeholder
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.
package layers

import (
	"reflect"
	"testing"

	"github.com/google/gopacket"
)

const eapolErrFmt = "%s packet processing failed:\ngot  :\n%#v\n\nwant :\n%#v\n\n"

// testPacketEAPOLKey is frame 87 in the capture:
// https://wiki.wireshark.org/SampleCaptures?action=AttachFile&do=get&target=wpa-Induction.pcap
// It's the first EAPOL-Key frame in the WPA 4-way handshake:
// 0x0000   02 03 00 75 02 00 8a 00 10 00 00 00 00 00 00 00  ...u............
// 0x0010   00 3e 8e 96 7d ac d9 60 32 4c ac 5b 6a a7 21 23  .>..}..`2L.[j.!#
// 0x0020   5b f5 7b 94 97 71 c8 67 98 9f 49 d0 4e d4 7c 69  [.{..q.g..I.N.|i
// 0x0030   33 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  3...............
// 0x0040   00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
// 0x0050   00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
// 0x0060   00 00 16 dd 14 00 0f ac 04 59 2d a8 80 96 c4 61  .........Y-....a
// 0x0070   da 24 6c 69 00 1e 87 7f 3d                       .$li....=

var testPacketEAPOLKey = []byte{
	0x02, 0x03, 0x00, 0x75, 0x02, 0x00, 0x8a, 0x00,
	0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x3e, 0x8e, 0x96, 0x7d, 0xac, 0xd9, 0x60,
	0x32, 0x4c, 0xac, 0x5b, 0x6a, 0xa7, 0x21, 0x23,
	0x5b, 0xf5, 0x7b, 0x94, 0x97, 0x71, 0xc8, 0x67,
	0x98, 0x9f, 0x49, 0xd0, 0x4e, 0xd4, 0x7c, 0x69,
	0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x16, 0xdd, 0x14, 0x00, 0x0f, 0xac,
	0x04, 0x59, 0x2d, 0xa8, 0x80, 0x96, 0xc4, 0x61,
	0xda, 0x24, 0x6c, 0x69, 0x00, 0x1e, 0x87, 0x7f,
	0x3d,
}

func TestPacketEAPOLKey(t *testing.T) {
	p := gopacket.NewPacket(testPacketEAPOLKey, LayerTypeEAPOL, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeEAPOL, LayerTypeEAPOLKey,
		LayerTypeDot11InformationElement}, t)

	{
		got := p.Layer(LayerTypeEAPOL).(*EAPOL)
		want := &EAPOL{
			BaseLayer: BaseLayer{
				Contents: testPacketEAPOLKey[:4],
				Payload:  testPacketEAPOLKey[4:],
			},
			Version: 2,
			Type:    EAPOLTypeKey,
			Length:  117,
		}
		if !reflect.DeepEqual(got, want) {
			t.Errorf(eapolErrFmt, "EAPOL", got, want)
		}
	}

	{
		got := p.Layer(LayerTypeEAPOLKey).(*EAPOLKey)
		want := &EAPOLKey{
			BaseLayer: BaseLayer{
				Contents: testPacketEAPOLKey[4 : 4+eapolKeyFrameLen],
				Payload:  testPacketEAPOLKey[4+eapolKeyFrameLen:],
			},
			KeyDescriptorType:    EAPOLKeyDescriptorTypeDot11,
			KeyDescriptorVersion: EAPOLKeyDescriptorVersionAESHMACSHA1,
			KeyType:              EAPOLKeyTypePairwise,
			KeyACK:               true,
			KeyLength:            16,
			Nonce: []byte{
				0x3e, 0x8e, 0x96, 0x7d, 0xac, 0xd9, 0x60, 0x32,
				0x4c, 0xac, 0x5b, 0x6a, 0xa7, 0x21, 0x23, 0x5b,
				0xf5, 0x7b, 0x94, 0x97, 0x71, 0xc8, 0x67, 0x98,
				0x9f, 0x49, 0xd0, 0x4e, 0xd4, 0x7c, 0x69, 0x33,
			},
			IV:            make([]byte, 16),
			MIC:           make([]byte, 16),
			KeyDataLength: 22,
		}
		if !reflect.DeepEqual(got, want) {
			t.Errorf(eapolErrFmt, "EAPOLKey", got, want)
		}
	}
	{
		got := p.Layer(LayerTypeDot11InformationElement).(*Dot11InformationElement)
		want := &Dot11InformationElement{
			BaseLayer: BaseLayer{
				Contents: testPacketEAPOLKey[4+eapolKeyFrameLen:],
				Payload:  []byte{},
			},
			ID:     Dot11InformationElementIDVendor,
			Length: 20,
			OUI:    []byte{0x00, 0x0f, 0xac, 0x04},
			Info: []byte{
				0x59, 0x2d, 0xa8, 0x80, 0x96, 0xc4, 0x61, 0xda,
				0x24, 0x6c, 0x69, 0x00, 0x1e, 0x87, 0x7f, 0x3d,
			},
		}
		if !reflect.DeepEqual(got, want) {
			t.Errorf(eapolErrFmt, "Dot11InformationElement", got, want)
		}
	}
}

func BenchmarkDecodePacketEAPOLKey(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(testPacketEAPOLKey, nil, gopacket.NoCopy)
	}
}
