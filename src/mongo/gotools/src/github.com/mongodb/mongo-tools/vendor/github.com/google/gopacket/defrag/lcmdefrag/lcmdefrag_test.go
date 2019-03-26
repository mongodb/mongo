// Copyright 2018 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package lcmdefrag

import (
	"testing"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

var (
	fragmentOne = []byte{
		0x4c, 0x43, 0x30, 0x33, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0d,
		0x00, 0x00, 0x00, 0x2d, 0x00, 0x00, 0x00, 0x02, 0x4c, 0x43, 0x4d, 0x5f,
		0x53, 0x45, 0x4c, 0x46, 0x5f, 0x54, 0x45, 0x53, 0x54, 0x00, 0x6c, 0x63,
		0x6d, 0x20, 0x73, 0x65, 0x6c, 0x66, 0x20, 0x74, 0x65, 0x73, 0x74,
	}

	fragmentTwo = []byte{
		0x4c, 0x43, 0x30, 0x33, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0d,
		0x00, 0x00, 0x00, 0x2d, 0x00, 0x01, 0x00, 0x02, 0x6c, 0x63, 0x6d, 0x20,
		0x73, 0x65, 0x6c, 0x66, 0x20, 0x74, 0x65, 0x73, 0x74,
	}

	completePacket = []byte{
		0x4c, 0x43, 0x30, 0x32, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x43, 0x4d, 0x5f,
		0x53, 0x45, 0x4c, 0x46, 0x5f, 0x54, 0x45, 0x53, 0x54, 0x00, 0x6c, 0x63,
		0x6d, 0x20, 0x73, 0x65, 0x6c, 0x66, 0x20, 0x74, 0x65, 0x73, 0x74,
	}
)

func TestOrderedLCMDefrag(t *testing.T) {
	defragmenter := NewLCMDefragmenter()
	var err error

	packet := gopacket.NewPacket(fragmentOne, layers.LayerTypeLCM, gopacket.NoCopy)
	lcm := packet.Layer(layers.LayerTypeLCM).(*layers.LCM)

	lcm, err = defragmenter.Defrag(lcm)
	if lcm != nil {
		t.Fatal("Returned incomplete LCM message.")
	}
	if err != nil {
		t.Fatal(err)
	}

	packet = gopacket.NewPacket(fragmentTwo, layers.LayerTypeLCM, gopacket.NoCopy)
	lcm = packet.Layer(layers.LayerTypeLCM).(*layers.LCM)

	lcm, err = defragmenter.Defrag(lcm)
	if lcm == nil {
		t.Fatal("Did not receive defragmented LCM message.")
	}
	if err != nil {
		t.Fatal(err)
	}
}

func TestUnorderedLCMDefrag(t *testing.T) {
	defragmenter := NewLCMDefragmenter()
	var err error

	packet := gopacket.NewPacket(fragmentTwo, layers.LayerTypeLCM, gopacket.NoCopy)
	lcm := packet.Layer(layers.LayerTypeLCM).(*layers.LCM)

	lcm, err = defragmenter.Defrag(lcm)
	if lcm != nil {
		t.Fatal("Returned incomplete LCM message.")
	}
	if err != nil {
		t.Fatal(err)
	}

	packet = gopacket.NewPacket(fragmentOne, layers.LayerTypeLCM, gopacket.NoCopy)
	lcm = packet.Layer(layers.LayerTypeLCM).(*layers.LCM)

	lcm, err = defragmenter.Defrag(lcm)
	if lcm == nil {
		t.Fatal("Did not receive defragmented LCM message.")
	}
	if err != nil {
		t.Fatal(err)
	}
}

func TestNonLCMDefrag(t *testing.T) {
	defragmenter := NewLCMDefragmenter()
	var err error

	packet := gopacket.NewPacket(completePacket, layers.LayerTypeLCM, gopacket.NoCopy)
	lcm := packet.Layer(layers.LayerTypeLCM).(*layers.LCM)

	lcm, err = defragmenter.Defrag(lcm)
	if lcm == nil {
		t.Fatal("Did not receive complete LCM message.")
	}
	if err != nil {
		t.Fatal(err)
	}
}
