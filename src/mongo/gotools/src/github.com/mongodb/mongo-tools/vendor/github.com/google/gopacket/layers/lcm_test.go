// Copyright 2018 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"encoding/hex"
	"testing"

	"github.com/google/gopacket"
)

var (
	fingerprint uint64 = 0x6c636d2073656c66

	shortPacket = []byte{
		0x4c, 0x43, 0x30, 0x32, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x43, 0x4d, 0x5f,
		0x53, 0x45, 0x4c, 0x46, 0x5f, 0x54, 0x45, 0x53, 0x54, 0x00, 0x6c, 0x63,
		0x6d, 0x20, 0x73, 0x65, 0x6c, 0x66, 0x20, 0x74, 0x65, 0x73, 0x74,
	}

	fragmentedPacket = []byte{
		0x4c, 0x43, 0x30, 0x33, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0d,
		0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x02, 0x4c, 0x43, 0x4d, 0x5f,
		0x53, 0x45, 0x4c, 0x46, 0x5f, 0x54, 0x45, 0x53, 0x54, 0x00, 0x6c, 0x63,
		0x6d, 0x20, 0x73, 0x65, 0x6c, 0x66, 0x20, 0x74, 0x65, 0x73, 0x74,
	}

	invalidPacket = []byte{
		0x4c, 0x43, 0x30, 0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	}

	expectedChannel = "LCM_SELF_TEST"
)

func TestLCMDecode(t *testing.T) {
	testShortLCM(t)
	testFragmentedLCM(t)
	testInvalidLCM(t)
}

func testShortLCM(t *testing.T) {
	lcm := &LCM{}

	err := lcm.DecodeFromBytes(shortPacket, gopacket.NilDecodeFeedback)
	if err != nil {
		t.Fatal(err)
	}

	if lcm.Magic != LCMShortHeaderMagic {
		t.Errorf("Expected LCM Magic %x, but decoded %x.\n",
			LCMShortHeaderMagic, lcm.Magic)
	}

	if lcm.SequenceNumber != 0x00 {
		t.Errorf("Expected an LCM Sequence Number of %x, but decoded %x.\n",
			0x00, lcm.SequenceNumber)
	}

	if lcm.ChannelName != expectedChannel {
		t.Errorf("Expected channel name %s but received %s\n",
			expectedChannel, lcm.ChannelName)
	}

	if lcm.Fragmented {
		t.Errorf("Misinterpreted non-fragmented packet as fragmented.")
	}

	for i, val := range lcm.LayerContents() {
		if val != shortPacket[i] {
			t.Errorf("\nLCM Payload: expected\n%sbut received\n%s",
				hex.Dump(shortPacket[:22]), hex.Dump(lcm.Payload()))
		}
	}

	for i, val := range lcm.Payload() {
		if val != shortPacket[i+22] {
			t.Errorf("\nLCM Payload: expected\n%sbut received\n%s",
				hex.Dump(shortPacket[22:]), hex.Dump(lcm.Payload()))
		}
	}
}

func testFragmentedLCM(t *testing.T) {
	lcm := LCM{}

	err := lcm.DecodeFromBytes(fragmentedPacket, gopacket.NilDecodeFeedback)
	if err != nil {
		t.Fatal(err)
	}

	if lcm.Magic != LCMFragmentedHeaderMagic {
		t.Errorf("Expected LCM Magic %x, but decoded %x.\n",
			LCMFragmentedHeaderMagic, lcm.Magic)
	}

	if lcm.SequenceNumber != 0x01 {
		t.Errorf("Expected an LCM Sequence Number of %x, but decoded %x.\n",
			0x01, lcm.SequenceNumber)
	}

	if lcm.PayloadSize != 0x0d {
		t.Errorf("Expected an LCM Payload Size of %x, but decoded %x.\n", 0x0d,
			lcm.PayloadSize)
	}

	if lcm.FragmentOffset != 0x2d {
		t.Errorf("Expected an LCM Fragment Offset of %x, but decoded %x.\n",
			0x2d, lcm.FragmentOffset)
	}

	if lcm.FragmentNumber != 0x00 {
		t.Errorf("Expected the first LCM fragment (%x), but decoded %x.\n",
			0x00, lcm.FragmentNumber)
	}

	if lcm.TotalFragments != 0x02 {
		t.Errorf("Expected two LCM fragments (%x), but decoded %x.\n", 0x02,
			lcm.TotalFragments)
	}

	if lcm.ChannelName != expectedChannel {
		t.Errorf("Expected LCM Channel Name %s but decoded %s\n",
			expectedChannel, lcm.ChannelName)
	}

	if !lcm.Fragmented {
		t.Errorf("Misinterpreted fragmented packet as non-fragmented.")
	}

	for i, val := range lcm.LayerContents() {
		if val != fragmentedPacket[i] {
			t.Errorf("\nLCM Payload: expected\n%sbut received\n%s",
				hex.Dump(fragmentedPacket[:22]), hex.Dump(lcm.Payload()))
		}
	}

	for i, val := range lcm.Payload() {
		if val != fragmentedPacket[i+34] {
			t.Errorf("\nLCM Payload: expected\n%sbut received\n%s",
				hex.Dump(fragmentedPacket[34:]), hex.Dump(lcm.Payload()))
		}
	}
}

func testInvalidLCM(t *testing.T) {
	lcm := LCM{}

	err := lcm.DecodeFromBytes(invalidPacket, gopacket.NilDecodeFeedback)
	if err == nil {
		t.Fatal("Did not detect LCM decode error.")
	}
}
