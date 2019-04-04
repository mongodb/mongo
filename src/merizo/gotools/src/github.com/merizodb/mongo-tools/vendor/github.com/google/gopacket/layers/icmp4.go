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
	"strconv"
)

type ICMPv4TypeCode uint16

const (
	ICMPv4TypeEchoReply              = 0
	ICMPv4TypeDestinationUnreachable = 3
	ICMPv4TypeSourceQuench           = 4
	ICMPv4TypeRedirect               = 5
	ICMPv4TypeEchoRequest            = 8
	ICMPv4TypeRouterAdvertisement    = 9
	ICMPv4TypeRouterSolicitation     = 10
	ICMPv4TypeTimeExceeded           = 11
	ICMPv4TypeParameterProblem       = 12
	ICMPv4TypeTimestampRequest       = 13
	ICMPv4TypeTimestampReply         = 14
	ICMPv4TypeInfoRequest            = 15
	ICMPv4TypeInfoReply              = 16
	ICMPv4TypeAddressMaskRequest     = 17
	ICMPv4TypeAddressMaskReply       = 18
)

// ICMPv4 is the layer for IPv4 ICMP packet data.
type ICMPv4 struct {
	BaseLayer
	TypeCode ICMPv4TypeCode
	Checksum uint16
	Id       uint16
	Seq      uint16
}

// LayerType returns gopacket.LayerTypeICMPv4
func (i *ICMPv4) LayerType() gopacket.LayerType { return LayerTypeICMPv4 }

func decodeICMPv4(data []byte, p gopacket.PacketBuilder) error {
	i := &ICMPv4{}
	err := i.DecodeFromBytes(data, p)
	if err != nil {
		return err
	}
	p.AddLayer(i)
	return p.NextDecoder(gopacket.LayerTypePayload)
}

var tooShort error = fmt.Errorf("icmp layer less than 8 bytes")

func (i *ICMPv4) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 8 {
		df.SetTruncated()
		return tooShort
	}
	i.TypeCode = ICMPv4TypeCode(binary.BigEndian.Uint16(data[:2]))
	i.Checksum = binary.BigEndian.Uint16(data[2:4])
	i.Id = binary.BigEndian.Uint16(data[4:6])
	i.Seq = binary.BigEndian.Uint16(data[6:8])
	i.BaseLayer = BaseLayer{data[:8], data[8:]}
	return nil
}

// SerializeTo writes the serialized form of this layer into the
// SerializationBuffer, implementing gopacket.SerializableLayer.
// See the docs for gopacket.SerializableLayer for more info.
func (i *ICMPv4) SerializeTo(b gopacket.SerializeBuffer, opts gopacket.SerializeOptions) error {
	bytes, err := b.PrependBytes(8)
	if err != nil {
		return err
	}
	binary.BigEndian.PutUint16(bytes, uint16(i.TypeCode))
	binary.BigEndian.PutUint16(bytes[4:], i.Id)
	binary.BigEndian.PutUint16(bytes[6:], i.Seq)
	if opts.ComputeChecksums {
		bytes[2] = 0
		bytes[3] = 0
		i.Checksum = tcpipChecksum(b.Bytes(), 0)
	}
	binary.BigEndian.PutUint16(bytes[2:], i.Checksum)
	return nil
}

func (i *ICMPv4) CanDecode() gopacket.LayerClass {
	return LayerTypeICMPv4
}

func (i *ICMPv4) NextLayerType() gopacket.LayerType {
	return gopacket.LayerTypePayload
}

func (a ICMPv4TypeCode) String() string {
	typ := uint8(a >> 8)
	code := uint8(a)
	var typeStr, codeStr string
	switch typ {
	case ICMPv4TypeDestinationUnreachable:
		typeStr = "DestinationUnreachable"
		switch code {
		case 0:
			codeStr = "Net"
		case 1:
			codeStr = "Host"
		case 2:
			codeStr = "Protocol"
		case 3:
			codeStr = "Port"
		case 4:
			codeStr = "FragmentationNeeded"
		case 5:
			codeStr = "SourceRoutingFailed"
		case 6:
			codeStr = "NetUnknown"
		case 7:
			codeStr = "HostUnknown"
		case 8:
			codeStr = "SourceIsolated"
		case 9:
			codeStr = "NetAdminProhibited"
		case 10:
			codeStr = "HostAdminProhibited"
		case 11:
			codeStr = "NetTOS"
		case 12:
			codeStr = "HostTOS"
		case 13:
			codeStr = "CommAdminProhibited"
		case 14:
			codeStr = "HostPrecedence"
		case 15:
			codeStr = "PrecedenceCutoff"
		}
	case ICMPv4TypeTimeExceeded:
		typeStr = "TimeExceeded"
		switch code {
		case 0:
			codeStr = "TTLExceeded"
		case 1:
			codeStr = "FragmentReassemblyTimeExceeded"
		}
	case ICMPv4TypeParameterProblem:
		typeStr = "ParameterProblem"
		switch code {
		case 0:
			codeStr = "PointerIndicatesError"
		case 1:
			codeStr = "MissingOption"
		case 2:
			codeStr = "BadLength"
		}
	case ICMPv4TypeSourceQuench:
		typeStr = "SourceQuench"
	case ICMPv4TypeRedirect:
		typeStr = "Redirect"
		switch code {
		case 0:
			codeStr = "Network"
		case 1:
			codeStr = "Host"
		case 2:
			codeStr = "TOS+Network"
		case 3:
			codeStr = "TOS+Host"
		}
	case ICMPv4TypeEchoRequest:
		typeStr = "EchoRequest"
	case ICMPv4TypeEchoReply:
		typeStr = "EchoReply"
	case ICMPv4TypeTimestampRequest:
		typeStr = "TimestampRequest"
	case ICMPv4TypeTimestampReply:
		typeStr = "TimestampReply"
	case ICMPv4TypeInfoRequest:
		typeStr = "InfoRequest"
	case ICMPv4TypeInfoReply:
		typeStr = "InfoReply"
	case ICMPv4TypeRouterSolicitation:
		typeStr = "RouterSolicitation"
	case ICMPv4TypeRouterAdvertisement:
		typeStr = "RouterAdvertisement"
	case ICMPv4TypeAddressMaskRequest:
		typeStr = "AddressMaskRequest"
	case ICMPv4TypeAddressMaskReply:
		typeStr = "AddressMaskReply"
	default:
		typeStr = strconv.Itoa(int(typ))
	}
	if codeStr == "" {
		codeStr = strconv.Itoa(int(code))
	}
	return fmt.Sprintf("%s(%s)", typeStr, codeStr)
}
