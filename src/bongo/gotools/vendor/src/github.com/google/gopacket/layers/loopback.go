// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"encoding/binary"
	"fmt"
	"github.com/google/gopacket"
)

// Loopback contains the header for loopback encapsulation.  This header is
// used by both BSD and OpenBSD style loopback decoding (pcap's DLT_NULL
// and DLT_LOOP, respectively).
type Loopback struct {
	BaseLayer
	Family ProtocolFamily
}

// LayerType returns LayerTypeLoopback.
func (l *Loopback) LayerType() gopacket.LayerType { return LayerTypeLoopback }

func decodeLoopback(data []byte, p gopacket.PacketBuilder) error {
	// The protocol could be either big-endian or little-endian, we're
	// not sure.  But we're PRETTY sure that the value is less than
	// 256, so we can check the first two bytes.
	var prot uint32
	if data[0] == 0 && data[1] == 0 {
		prot = binary.BigEndian.Uint32(data[:4])
	} else {
		prot = binary.LittleEndian.Uint32(data[:4])
	}
	if prot > 0xFF {
		return fmt.Errorf("Invalid loopback protocol %q", data[:4])
	}
	l := &Loopback{
		BaseLayer: BaseLayer{data[:4], data[4:]},
		Family:    ProtocolFamily(prot),
	}
	p.AddLayer(l)
	return p.NextDecoder(l.Family)
}
