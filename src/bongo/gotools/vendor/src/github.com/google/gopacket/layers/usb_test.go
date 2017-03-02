// Copyright 2014, Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	_ "fmt"
	"github.com/google/gopacket"
	"reflect"
	"testing"
)

// Generator python layers/test_creator.py --link_type USB --name USB dongle.pcap
// http://wiki.wireshark.org/SampleCaptures#Sample_Captures

// testPacketUSB0 is the packet:
//   02:41:04.689546 INTERRUPT COMPLETE to 2:1:1
//   	0x0000:  0038 4a3b 0088 ffff 4301 8101 0200 2d00  .8J;....C.....-.
//   	0x0010:  c0d3 5b50 0000 0000 8a85 0a00 0000 0000  ..[P............
//   	0x0020:  0100 0000 0100 0000 0000 0000 0000 0000  ................
//   	0x0030:  8000 0000 0000 0000 0002 0000 0000 0000  ................
//   	0x0040:  04                                       .
var testPacketUSB0 = []byte{
	0x00, 0x38, 0x4a, 0x3b, 0x00, 0x88, 0xff, 0xff, 0x43, 0x01, 0x81, 0x01, 0x02, 0x00, 0x2d, 0x00,
	0xc0, 0xd3, 0x5b, 0x50, 0x00, 0x00, 0x00, 0x00, 0x8a, 0x85, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x04,
}

func TestPacketUSB0(t *testing.T) {
	p := gopacket.NewPacket(testPacketUSB0, LinkTypeLinuxUSB, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Error("Failed to decode packet:", p.ErrorLayer().Error())
	}
	checkLayers(p, []gopacket.LayerType{LayerTypeUSB, LayerTypeUSBInterrupt}, t)

	if got, ok := p.Layer(LayerTypeUSB).(*USB); ok {
		want := &USB{
			BaseLayer: BaseLayer{
				Contents: []uint8{0x0, 0x38, 0x4a, 0x3b, 0x0, 0x88, 0xff, 0xff, 0x43, 0x1, 0x81, 0x1, 0x2, 0x0, 0x2d, 0x0, 0xc0, 0xd3, 0x5b, 0x50, 0x0, 0x0, 0x0, 0x0, 0x8a, 0x85, 0xa, 0x0, 0x0, 0x0, 0x0, 0x0, 0x1, 0x0, 0x0, 0x0, 0x1, 0x0, 0x0, 0x0},
				Payload:  []uint8{0x4},
			},
			ID:             0xffff88003b4a3800,
			EventType:      USBEventTypeComplete,
			TransferType:   USBTransportTypeInterrupt,
			Direction:      0x1,
			EndpointNumber: 0x1,
			DeviceAddress:  0x1,
			BusID:          0x2,
			TimestampSec:   1348195264,
			TimestampUsec:  689546,
			Setup:          false,
			Data:           true,
			Status:         0,
			UrbLength:      0x1,
			UrbDataLength:  0x1,
		}

		if !reflect.DeepEqual(got, want) {
			t.Errorf("USB packet processing failed:\ngot  :\n%#v\n\nwant :\n%#v\n\n", got, want)
		}
	}

}
func BenchmarkDecodePacketUSB0(b *testing.B) {
	for i := 0; i < b.N; i++ {
		gopacket.NewPacket(testPacketUSB0, LinkTypeLinuxUSB, gopacket.NoCopy)
	}
}
