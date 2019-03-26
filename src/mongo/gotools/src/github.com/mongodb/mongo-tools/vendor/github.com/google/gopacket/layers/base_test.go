// Copyright 2012, Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// This file contains some test helper functions.

package layers

import (
	"testing"

	"github.com/google/gopacket"
)

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func checkLayers(p gopacket.Packet, want []gopacket.LayerType, t *testing.T) {
	layers := p.Layers()
	t.Log("Checking packet layers, want", want)
	for _, l := range layers {
		t.Logf("  Got layer %v, %d bytes, payload of %d bytes", l.LayerType(),
			len(l.LayerContents()), len(l.LayerPayload()))
	}
	t.Log(p)
	if len(layers) < len(want) {
		t.Errorf("  Number of layers mismatch: got %d want %d", len(layers),
			len(want))
		return
	}
	for i, l := range want {
		if l == gopacket.LayerTypePayload {
			// done matching layers
			return
		}

		if layers[i].LayerType() != l {
			t.Errorf("  Layer %d mismatch: got %v want %v", i,
				layers[i].LayerType(), l)
		}
	}
}

// Checks that when a serialized version of p is decoded, p and the serialized version of p are the same.
// Does not work for packets where the order of options can change, like icmpv6 router advertisements, dhcpv6, etc.
func checkSerialization(p gopacket.Packet, t *testing.T) {
	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{
		ComputeChecksums: false,
		FixLengths:       false,
	}
	if err := gopacket.SerializePacket(buf, opts, p); err != nil {
		t.Error("Failed to encode packet:", err)
	}

	p2 := gopacket.NewPacket(buf.Bytes(), LinkTypeEthernet, gopacket.Default)
	if p2.ErrorLayer() != nil {
		t.Error("Failed to decode the re-encoded packet:", p2.ErrorLayer().Error())
	}

	if p2.Dump() != p.Dump() {
		t.Errorf("The decoded and the re-encoded packet are different!\nDecoded:\n%s\n Re-Encoded:\n%s", p.Dump(), p2.Dump())
	}
}
