// Copyright 2012 Google, Inc. All rights reserved.
// Copyright 2009-2011 Andreas Krennmair. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"encoding/binary"
	"github.com/google/gopacket"
	"net"
	"time"
)

type IGMPType uint8

// IGMP is the packet structure for IGMP messages.
type IGMP struct {
	BaseLayer
	Type            IGMPType
	MaxResponseTime time.Duration
	Checksum        uint16
	GroupAddress    net.IP
	// The following are used only by IGMPv3
	SupressRouterProcessing bool
	RobustnessValue         uint8
	IntervalTime            time.Duration
	SourceAddresses         []net.IP
}

// LayerType returns LayerTypeIGMP
func (i *IGMP) LayerType() gopacket.LayerType { return LayerTypeIGMP }

// igmpTimeDecode decodes the duration created by the given byte, using the
// algorithm in http://www.rfc-base.org/txt/rfc-3376.txt section 4.1.1.
func igmpTimeDecode(t uint8) time.Duration {
	if t&0x80 == 0 {
		return time.Millisecond * 100 * time.Duration(t)
	}
	mant := (t & 0x70) >> 4
	exp := t & 0x0F
	return time.Millisecond * 100 * time.Duration((mant|0x10)<<(exp+3))
}

// DecodeFromBytes decodes the given bytes into this layer.
func (i *IGMP) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	i.Type = IGMPType(data[0])
	i.MaxResponseTime = igmpTimeDecode(data[1])
	i.Checksum = binary.BigEndian.Uint16(data[2:4])
	i.GroupAddress = net.IP(data[4:8])
	if i.Type == 0x11 && len(data) > 8 {
		i.SupressRouterProcessing = data[8]&0x8 != 0
		i.RobustnessValue = data[8] & 0x7
		i.IntervalTime = igmpTimeDecode(data[9])
		numSources := int(binary.BigEndian.Uint16(data[10:12]))
		for j := 0; j < numSources; j++ {
			i.SourceAddresses = append(i.SourceAddresses, net.IP(data[12+j*4:16+j*4]))
		}
	} else {
		i.SupressRouterProcessing = false
		i.RobustnessValue = 0
		i.IntervalTime = 0
		i.SourceAddresses = nil
	}
	return nil
}

// CanDecode returns the set of layer types that this DecodingLayer can decode.
func (i *IGMP) CanDecode() gopacket.LayerClass {
	return LayerTypeIGMP
}

// NextLayerType returns the layer type contained by this DecodingLayer.
func (i *IGMP) NextLayerType() gopacket.LayerType {
	return gopacket.LayerTypeZero
}

func decodeIGMP(data []byte, p gopacket.PacketBuilder) error {
	i := &IGMP{}
	return decodingLayerDecoder(i, data, p)
}
