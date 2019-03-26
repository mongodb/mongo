// Copyright 2016 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.
//
//******************************************************************************

package layers

import (
	"crypto/rand"
	"github.com/google/gopacket"
	"io"
	"reflect"
	"testing"
)

//******************************************************************************

// checkNTP() uses the ntp.go code to analyse the packet bytes as an NTP UDP
// packet and generate an NTP object. It then compares the generated NTP object
// with the one provided and throws an error if there is any difference.
// The desc argument is output with any failure message to identify the test.
func checkNTP(desc string, t *testing.T, packetBytes []byte, pExpectedNTP *NTP) {

	// Analyse the packet bytes, yielding a new packet object p.
	p := gopacket.NewPacket(packetBytes, LinkTypeEthernet, gopacket.Default)
	if p.ErrorLayer() != nil {
		t.Errorf("Failed to decode packet %s: %v", desc, p.ErrorLayer().Error())
	}

	// Ensure that the packet analysis yielded the correct set of layers:
	//    Link Layer        = Ethernet.
	//    Network Layer     = IPv4.
	//    Transport Layer   = UDP.
	//    Application Layer = NTP.
	checkLayers(p, []gopacket.LayerType{
		LayerTypeEthernet,
		LayerTypeIPv4,
		LayerTypeUDP,
		LayerTypeNTP}, t)

	// Select the Application (NTP) layer.
	pResultNTP, ok := p.ApplicationLayer().(*NTP)
	if !ok {
		t.Error("No NTP layer type found in packet in " + desc + ".")
	}

	// Compare the generated NTP object with the expected NTP object.
	if !reflect.DeepEqual(pResultNTP, pExpectedNTP) {
		t.Errorf("NTP packet processing failed for packet "+desc+
			":\ngot  :\n%#v\n\nwant :\n%#v\n\n", pResultNTP, pExpectedNTP)
	}
	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{}
	err := pResultNTP.SerializeTo(buf, opts)
	if err != nil {
		t.Error(err)
	}
	if !reflect.DeepEqual(pResultNTP.BaseLayer.Contents, buf.Bytes()) {
		t.Errorf("NTP packet serialization failed for packet "+desc+
			":\ngot  :\n%x\n\nwant :\n%x\n\n", buf.Bytes(), packetBytes)
	}
}

//******************************************************************************

func TestNTPOne(t *testing.T) {

	// This test packet is the first NTP packet in the NTP sample capture
	// pcap file NTP_sync.pcap on the Wireshark sample captures page:
	//
	//    https://wiki.wireshark.org/SampleCaptures
	//    https://wiki.wireshark.org/SampleCaptures?action=AttachFile&do=get&target=NTP_sync.pcap
	var testPacketNTP = []byte{
		0x00, 0x0c, 0x41, 0x82, 0xb2, 0x53, 0x00, 0xd0,
		0x59, 0x6c, 0x40, 0x4e, 0x08, 0x00, 0x45, 0x00,
		0x00, 0x4c, 0x0a, 0x42, 0x00, 0x00, 0x80, 0x11,
		0xb5, 0xfa, 0xc0, 0xa8, 0x32, 0x32, 0x43, 0x81,
		0x44, 0x09, 0x00, 0x7b, 0x00, 0x7b, 0x00, 0x38,
		0xf8, 0xd2, 0xd9, 0x00, 0x0a, 0xfa, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x01, 0x02, 0x90, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0xc5, 0x02, 0x04, 0xec, 0xec, 0x42,
		0xee, 0x92,
	}

	// Assemble the NTP object that we expect to emerge from this test.
	pExpectedNTP := &NTP{
		BaseLayer: BaseLayer{
			Contents: []byte{0xd9, 0x0, 0xa, 0xfa, 0x0, 0x0, 0x0, 0x0, 0x0,
				0x1, 0x2, 0x90, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
				0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0,
				0x0, 0x0, 0x0, 0x0, 0x0, 0xc5, 0x2, 0x4, 0xec, 0xec, 0x42, 0xee, 0x92},
			Payload: nil,
		},
		LeapIndicator:      3,
		Version:            3,
		Mode:               1,
		Stratum:            0,
		Poll:               10,
		Precision:          -6,
		RootDelay:          0,
		RootDispersion:     0x10290,
		ReferenceID:        0,
		ReferenceTimestamp: 0,
		OriginTimestamp:    0,
		ReceiveTimestamp:   0,
		TransmitTimestamp:  0xc50204ecec42ee92,
		ExtensionBytes:     []byte{},
	}

	checkNTP("test01", t, testPacketNTP, pExpectedNTP)
}

//******************************************************************************

func TestNTPTwo(t *testing.T) {

	// This test packet is packet #18 in the NTP sample capture
	// pcap file NTP_sync.pcap on the Wireshark sample captures page:
	//
	//    https://wiki.wireshark.org/SampleCaptures
	//    https://wiki.wireshark.org/SampleCaptures?action=AttachFile&do=get&target=NTP_sync.pcap
	//
	// This packet was chosen because it is the first NTP packet after the first
	// NTP packet that has non-zero timestamps.

	var testPacketNTP = []byte{
		0x00, 0xd0, 0x59, 0x6c, 0x40, 0x4e, 0x00, 0x0c,
		0x41, 0x82, 0xb2, 0x53, 0x08, 0x00, 0x45, 0x00,
		0x00, 0x4c, 0x32, 0x46, 0x40, 0x00, 0x2f, 0x11,
		0xa8, 0x18, 0x45, 0x2c, 0x39, 0x3c, 0xc0, 0xa8,
		0x32, 0x32, 0x00, 0x7b, 0x00, 0x7b, 0x00, 0x38,
		0x09, 0x58, 0x1a, 0x03, 0x0a, 0xee, 0x00, 0x00,
		0x1b, 0xf7, 0x00, 0x00, 0x14, 0xec, 0x51, 0xae,
		0x80, 0xb7, 0xc5, 0x02, 0x03, 0x4c, 0x8d, 0x0e,
		0x66, 0xcb, 0xc5, 0x02, 0x04, 0xec, 0xec, 0x42,
		0xee, 0x92, 0xc5, 0x02, 0x04, 0xeb, 0xcf, 0x49,
		0x59, 0xe6, 0xc5, 0x02, 0x04, 0xeb, 0xcf, 0x4c,
		0x6e, 0x6e,
	}

	// Assemble the NTP object that we expect to emerge from this test.
	pExpectedNTP := &NTP{
		BaseLayer: BaseLayer{
			Contents: []byte{0x1a, 0x03, 0x0a, 0xee, 0x00, 0x00,
				0x1b, 0xf7, 0x00, 0x00, 0x14, 0xec, 0x51, 0xae,
				0x80, 0xb7, 0xc5, 0x02, 0x03, 0x4c, 0x8d, 0x0e,
				0x66, 0xcb, 0xc5, 0x02, 0x04, 0xec, 0xec, 0x42,
				0xee, 0x92, 0xc5, 0x02, 0x04, 0xeb, 0xcf, 0x49,
				0x59, 0xe6, 0xc5, 0x02, 0x04, 0xeb, 0xcf, 0x4c,
				0x6e, 0x6e},
			Payload: nil,
		},
		LeapIndicator:      0,
		Version:            3,
		Mode:               2,
		Stratum:            3,
		Poll:               10,
		Precision:          -18,
		RootDelay:          0x1bf7,
		RootDispersion:     0x14ec,
		ReferenceID:        0x51ae80b7,
		ReferenceTimestamp: 0xc502034c8d0e66cb,
		OriginTimestamp:    0xc50204ecec42ee92,
		ReceiveTimestamp:   0xc50204ebcf4959e6,
		TransmitTimestamp:  0xc50204ebcf4c6e6e,
		ExtensionBytes:     []byte{},
	}

	checkNTP("test02", t, testPacketNTP, pExpectedNTP)
}

//******************************************************************************

func TestNTPThree(t *testing.T) {

	// This test packet is packet #19 in the NTP sample capture
	// pcap file NTP_sync.pcap on the Wireshark sample captures page:
	//
	//    https://wiki.wireshark.org/SampleCaptures
	//    https://wiki.wireshark.org/SampleCaptures?action=AttachFile&do=get&target=NTP_sync.pcap

	var testPacketNTP = []byte{
		0x00, 0xd0, 0x59, 0x6c, 0x40, 0x4e, 0x00, 0x0c,
		0x41, 0x82, 0xb2, 0x53, 0x08, 0x00, 0x45, 0x00,
		0x00, 0x4c, 0x00, 0x00, 0x40, 0x00, 0x30, 0x11,
		0x74, 0x65, 0x18, 0x7b, 0xca, 0xe6, 0xc0, 0xa8,
		0x32, 0x32, 0x00, 0x7b, 0x00, 0x7b, 0x00, 0x38,
		0x44, 0x05, 0x1a, 0x02, 0x0a, 0xec, 0x00, 0x00,
		0x07, 0xc3, 0x00, 0x00, 0x2f, 0x80, 0xc6, 0x1e,
		0x5c, 0x02, 0xc5, 0x01, 0xf9, 0x95, 0x42, 0x50,
		0x82, 0xcf, 0xc5, 0x02, 0x04, 0xec, 0xec, 0x42,
		0xee, 0x92, 0xc5, 0x02, 0x04, 0xeb, 0xd2, 0x35,
		0x2e, 0xb5, 0xc5, 0x02, 0x04, 0xeb, 0xd2, 0x35,
		0xd6, 0x7b,
	}

	// Assemble the NTP object that we expect to emerge from this test.
	pExpectedNTP := &NTP{
		BaseLayer: BaseLayer{
			Contents: []byte{0x1a, 0x02, 0x0a, 0xec, 0x00, 0x00,
				0x07, 0xc3, 0x00, 0x00, 0x2f, 0x80, 0xc6, 0x1e,
				0x5c, 0x02, 0xc5, 0x01, 0xf9, 0x95, 0x42, 0x50,
				0x82, 0xcf, 0xc5, 0x02, 0x04, 0xec, 0xec, 0x42,
				0xee, 0x92, 0xc5, 0x02, 0x04, 0xeb, 0xd2, 0x35,
				0x2e, 0xb5, 0xc5, 0x02, 0x04, 0xeb, 0xd2, 0x35,
				0xd6, 0x7b},
			Payload: nil,
		},
		LeapIndicator:      0,
		Version:            3,
		Mode:               2,
		Stratum:            2,
		Poll:               10,
		Precision:          -20,
		RootDelay:          0x7c3,
		RootDispersion:     0x2f80,
		ReferenceID:        0xc61e5c02,
		ReferenceTimestamp: 0xc501f995425082cf,
		OriginTimestamp:    0xc50204ecec42ee92,
		ReceiveTimestamp:   0xc50204ebd2352eb5,
		TransmitTimestamp:  0xc50204ebd235d67b,
		ExtensionBytes:     []byte{},
	}

	checkNTP("test03", t, testPacketNTP, pExpectedNTP)
}

//******************************************************************************

// TestNTPIsomorphism tests whether random data gets parsed into NTP layer and
// gets serialized back from it to the same value.
func TestNTPIsomorphism(t *testing.T) {
	NTPData := make([]byte, ntpMinimumRecordSizeInBytes+7)
	_, err := io.ReadFull(rand.Reader, NTPData)
	if err != nil {
		t.Error(err)
	}
	ntpLayer := &NTP{}
	err = ntpLayer.DecodeFromBytes(NTPData, gopacket.NilDecodeFeedback)
	if err != nil {
		t.Error(err)
	}
	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{}
	err = ntpLayer.SerializeTo(buf, opts)
	if err != nil {
		t.Error(err)
	}
	if !reflect.DeepEqual(NTPData, buf.Bytes()) {
		t.Errorf("NTP packet is not isomorphic:\ngot  :\n%x\n\nwant :\n%x\n\n", buf.Bytes(), NTPData)
	}
}
