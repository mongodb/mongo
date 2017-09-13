// Copyright 2012 Google, Inc. All rights reserved.
// Copyright 2009-2011 Andreas Krennmair. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"encoding/binary"
	"fmt"
	"github.com/google/gopacket"
	"reflect"
)

const (
	// The following are from RFC 4443
	ICMPv6TypeDestinationUnreachable = 1
	ICMPv6TypePacketTooBig           = 2
	ICMPv6TypeTimeExceeded           = 3
	ICMPv6TypeParameterProblem       = 4
	ICMPv6TypeEchoRequest            = 128
	ICMPv6TypeEchoReply              = 129
	// The following are from RFC 4861
	ICMPv6TypeRouterSolicitation    = 133
	ICMPv6TypeRouterAdvertisement   = 134
	ICMPv6TypeNeighborSolicitation  = 135
	ICMPv6TypeNeighborAdvertisement = 136
	ICMPv6TypeRedirect              = 137
)

type icmpv6TypeCodeInfoStruct struct {
	typeStr string
	codeStr *map[uint8]string
}

var (
	icmpv6TypeCodeInfo = map[uint8]icmpv6TypeCodeInfoStruct{
		1: icmpv6TypeCodeInfoStruct{
			"DestinationUnreachable", &map[uint8]string{
				0: "NoRouteToDst",
				1: "AdminProhibited",
				2: "BeyondScopeOfSrc",
				3: "AddressUnreachable",
				4: "PortUnreachable",
				5: "SrcAddressFailedPolicy",
				6: "RejectRouteToDst",
			},
		},
		2: icmpv6TypeCodeInfoStruct{
			"PacketTooBig", nil,
		},
		3: icmpv6TypeCodeInfoStruct{
			"TimeExceeded", &map[uint8]string{
				0: "HopLimitExceeded",
				1: "FragmentReassemblyTimeExceeded",
			},
		},
		4: icmpv6TypeCodeInfoStruct{
			"ParameterProblem", &map[uint8]string{
				0: "ErroneousHeaderField",
				1: "UnrecognizedNextHeader",
				2: "UnrecognizedNextHeader",
			},
		},
		128: icmpv6TypeCodeInfoStruct{
			"EchoRequest", nil,
		},
		129: icmpv6TypeCodeInfoStruct{
			"EchoReply", nil,
		},
		133: icmpv6TypeCodeInfoStruct{
			"RouterSolicitation", nil,
		},
		134: icmpv6TypeCodeInfoStruct{
			"RouterAdvertisement", nil,
		},
		135: icmpv6TypeCodeInfoStruct{
			"NeighborSolicitation", nil,
		},
		136: icmpv6TypeCodeInfoStruct{
			"NeighborAdvertisement", nil,
		},
		137: icmpv6TypeCodeInfoStruct{
			"Redirect", nil,
		},
	}
)

type ICMPv6TypeCode uint16

// Type returns the ICMPv6 type field.
func (a ICMPv6TypeCode) Type() uint8 {
	return uint8(a >> 8)
}

// Code returns the ICMPv6 code field.
func (a ICMPv6TypeCode) Code() uint8 {
	return uint8(a)
}

func (a ICMPv6TypeCode) String() string {
	t, c := a.Type(), a.Code()
	strInfo, ok := icmpv6TypeCodeInfo[t]
	if !ok {
		// Unknown ICMPv6 type field
		return fmt.Sprintf("%d(%d)", t, c)
	}
	typeStr := strInfo.typeStr
	if strInfo.codeStr == nil && c == 0 {
		// The ICMPv6 type does not make use of the code field
		return fmt.Sprintf("%s", strInfo.typeStr)
	}
	if strInfo.codeStr == nil && c != 0 {
		// The ICMPv6 type does not make use of the code field, but it is present anyway
		return fmt.Sprintf("%s(Code: %d)", typeStr, c)
	}
	codeStr, ok := (*strInfo.codeStr)[c]
	if !ok {
		// We don't know this ICMPv6 code; print the numerical value
		return fmt.Sprintf("%s(Code: %d)", typeStr, c)
	}
	return fmt.Sprintf("%s(%s)", typeStr, codeStr)
}

func (a ICMPv6TypeCode) GoString() string {
	t := reflect.TypeOf(a)
	return fmt.Sprintf("%s(%d, %d)", t.String(), a.Type(), a.Code())
}

// SerializeTo writes the ICMPv6TypeCode value to the 'bytes' buffer.
func (a ICMPv6TypeCode) SerializeTo(bytes []byte) {
	binary.BigEndian.PutUint16(bytes, uint16(a))
}

// CreateICMPv6TypeCode is a convenience function to create an ICMPv6TypeCode
// gopacket type from the ICMPv6 type and code values.
func CreateICMPv6TypeCode(typ uint8, code uint8) ICMPv6TypeCode {
	return ICMPv6TypeCode(binary.BigEndian.Uint16([]byte{typ, code}))
}

// ICMPv6 is the layer for IPv6 ICMP packet data
type ICMPv6 struct {
	BaseLayer
	TypeCode  ICMPv6TypeCode
	Checksum  uint16
	TypeBytes []byte
	tcpipchecksum
}

// LayerType returns LayerTypeICMPv6.
func (i *ICMPv6) LayerType() gopacket.LayerType { return LayerTypeICMPv6 }

// DecodeFromBytes decodes the given bytes into this layer.
func (i *ICMPv6) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	i.TypeCode = CreateICMPv6TypeCode(data[0], data[1])
	i.Checksum = binary.BigEndian.Uint16(data[2:4])
	i.TypeBytes = data[4:8]
	i.BaseLayer = BaseLayer{data[:8], data[8:]}
	return nil
}

// SerializeTo writes the serialized form of this layer into the
// SerializationBuffer, implementing gopacket.SerializableLayer.
// See the docs for gopacket.SerializableLayer for more info.
func (i *ICMPv6) SerializeTo(b gopacket.SerializeBuffer, opts gopacket.SerializeOptions) error {
	if i.TypeBytes == nil {
		i.TypeBytes = lotsOfZeros[:4]
	} else if len(i.TypeBytes) != 4 {
		return fmt.Errorf("invalid type bytes for ICMPv6 packet: %v", i.TypeBytes)
	}
	bytes, err := b.PrependBytes(8)
	if err != nil {
		return err
	}
	i.TypeCode.SerializeTo(bytes)
	copy(bytes[4:8], i.TypeBytes)
	if opts.ComputeChecksums {
		bytes[2] = 0
		bytes[3] = 0
		csum, err := i.computeChecksum(b.Bytes(), IPProtocolICMPv6)
		if err != nil {
			return err
		}
		i.Checksum = csum
	}
	binary.BigEndian.PutUint16(bytes[2:], i.Checksum)
	return nil
}

// CanDecode returns the set of layer types that this DecodingLayer can decode.
func (i *ICMPv6) CanDecode() gopacket.LayerClass {
	return LayerTypeICMPv6
}

// NextLayerType returns the layer type contained by this DecodingLayer.
func (i *ICMPv6) NextLayerType() gopacket.LayerType {
	return gopacket.LayerTypePayload
}

func decodeICMPv6(data []byte, p gopacket.PacketBuilder) error {
	i := &ICMPv6{}
	return decodingLayerDecoder(i, data, p)
}
