// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"encoding/binary"
	"github.com/google/gopacket"
)

// GRE is a Generic Routing Encapsulation header.
type GRE struct {
	BaseLayer
	ChecksumPresent, RoutingPresent, KeyPresent, SeqPresent, StrictSourceRoute bool
	RecursionControl, Flags, Version                                           uint8
	Protocol                                                                   EthernetType
	Checksum, Offset                                                           uint16
	Key, Seq                                                                   uint32
	*GRERouting
}

// GRERouting is GRE routing information, present if the RoutingPresent flag is
// set.
type GRERouting struct {
	AddressFamily        uint16
	SREOffset, SRELength uint8
	RoutingInformation   []byte
	Next                 *GRERouting
}

// LayerType returns gopacket.LayerTypeGRE.
func (g *GRE) LayerType() gopacket.LayerType { return LayerTypeGRE }

// DecodeFromBytes decodes the given bytes into this layer.
func (g *GRE) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	g.ChecksumPresent = data[0]&0x80 != 0
	g.RoutingPresent = data[0]&0x40 != 0
	g.KeyPresent = data[0]&0x20 != 0
	g.SeqPresent = data[0]&0x10 != 0
	g.StrictSourceRoute = data[0]&0x08 != 0
	g.RecursionControl = data[0] & 0x7
	g.Flags = data[1] >> 3
	g.Version = data[1] & 0x7
	g.Protocol = EthernetType(binary.BigEndian.Uint16(data[2:4]))
	offset := 4
	if g.ChecksumPresent || g.RoutingPresent {
		g.Checksum = binary.BigEndian.Uint16(data[offset : offset+2])
		g.Offset = binary.BigEndian.Uint16(data[offset+2 : offset+4])
		offset += 4
	}
	if g.KeyPresent {
		g.Key = binary.BigEndian.Uint32(data[offset : offset+4])
		offset += 4
	}
	if g.SeqPresent {
		g.Seq = binary.BigEndian.Uint32(data[offset : offset+4])
		offset += 4
	}
	if g.RoutingPresent {
		tail := &g.GRERouting
		for {
			sre := &GRERouting{
				AddressFamily: binary.BigEndian.Uint16(data[offset : offset+2]),
				SREOffset:     data[offset+2],
				SRELength:     data[offset+3],
			}
			sre.RoutingInformation = data[offset+4 : offset+4+int(sre.SRELength)]
			offset += 4 + int(sre.SRELength)
			if sre.AddressFamily == 0 && sre.SRELength == 0 {
				break
			}
			(*tail) = sre
			tail = &sre.Next
		}
	}
	g.BaseLayer = BaseLayer{data[:offset], data[offset:]}
	return nil
}

// CanDecode returns the set of layer types that this DecodingLayer can decode.
func (g *GRE) CanDecode() gopacket.LayerClass {
	return LayerTypeGRE
}

// NextLayerType returns the layer type contained by this DecodingLayer.
func (g *GRE) NextLayerType() gopacket.LayerType {
	return g.Protocol.LayerType()
}

func decodeGRE(data []byte, p gopacket.PacketBuilder) error {
	g := &GRE{}
	return decodingLayerDecoder(g, data, p)
}
