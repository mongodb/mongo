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
	"net"
)

const (
	IPv6HopByHopOptionJumbogram = 0xC2 // RFC 2675
)

const (
	ipv6MaxPayloadLength = 65535
)

// IPv6 is the layer for the IPv6 header.
type IPv6 struct {
	// http://www.networksorcery.com/enp/protocol/ipv6.htm
	BaseLayer
	Version      uint8
	TrafficClass uint8
	FlowLabel    uint32
	Length       uint16
	NextHeader   IPProtocol
	HopLimit     uint8
	SrcIP        net.IP
	DstIP        net.IP
	HopByHop     *IPv6HopByHop
	// hbh will be pointed to by HopByHop if that layer exists.
	hbh IPv6HopByHop
}

// LayerType returns LayerTypeIPv6
func (i *IPv6) LayerType() gopacket.LayerType { return LayerTypeIPv6 }

func (i *IPv6) NetworkFlow() gopacket.Flow {
	return gopacket.NewFlow(EndpointIPv6, i.SrcIP, i.DstIP)
}

// Search for Jumbo Payload TLV in IPv6HopByHop and return (length, true) if found
func getIPv6HopByHopJumboLength(hopopts *IPv6HopByHop) (uint32, bool, error) {
	var tlv *IPv6HopByHopOption

	for _, t := range hopopts.Options {
		if t.OptionType == IPv6HopByHopOptionJumbogram {
			tlv = t
			break
		}
	}
	if tlv == nil {
		// Not found
		return 0, false, nil
	}
	if len(tlv.OptionData) != 4 {
		return 0, false, fmt.Errorf("Jumbo length TLV data must have length 4")
	}
	l := binary.BigEndian.Uint32(tlv.OptionData)
	if l <= ipv6MaxPayloadLength {
		return 0, false, fmt.Errorf("Jumbo length cannot be less than %d", ipv6MaxPayloadLength+1)
	}
	// Found
	return l, true, nil
}

// Adds zero-valued Jumbo TLV to IPv6 header if it does not exist
// (if necessary add hop-by-hop header)
func addIPv6JumboOption(ip6 *IPv6) {
	var tlv *IPv6HopByHopOption

	if ip6.HopByHop == nil {
		// Add IPv6 HopByHop
		ip6.HopByHop = &IPv6HopByHop{}
		ip6.HopByHop.NextHeader = ip6.NextHeader
		ip6.HopByHop.HeaderLength = 0
		ip6.NextHeader = IPProtocolIPv6HopByHop
	}
	for _, t := range ip6.HopByHop.Options {
		if t.OptionType == IPv6HopByHopOptionJumbogram {
			tlv = t
			break
		}
	}
	if tlv == nil {
		// Add Jumbo TLV
		tlv = &IPv6HopByHopOption{}
		ip6.HopByHop.Options = append(ip6.HopByHop.Options, tlv)
	}
	tlv.SetJumboLength(0)
}

// Set jumbo length in serialized IPv6 payload (starting with HopByHop header)
func setIPv6PayloadJumboLength(hbh []byte) error {
	pLen := len(hbh)
	if pLen < 8 {
		//HopByHop is minimum 8 bytes
		return fmt.Errorf("Invalid IPv6 payload (length %d)", pLen)
	}
	hbhLen := int((hbh[1] + 1) * 8)
	if hbhLen > pLen {
		return fmt.Errorf("Invalid hop-by-hop length (length: %d, payload: %d", hbhLen, pLen)
	}
	offset := 2 //start with options
	for offset < hbhLen {
		opt := hbh[offset]
		if opt == 0 {
			//Pad1
			offset += 1
			continue
		}
		optLen := int(hbh[offset+1])
		if opt == IPv6HopByHopOptionJumbogram {
			if optLen == 4 {
				binary.BigEndian.PutUint32(hbh[offset+2:], uint32(pLen))
				return nil
			}
			return fmt.Errorf("Jumbo TLV too short (%d bytes)", optLen)
		}
		offset += 2 + optLen
	}
	return fmt.Errorf("Jumbo TLV not found")
}

// SerializeTo writes the serialized form of this layer into the
// SerializationBuffer, implementing gopacket.SerializableLayer.
// See the docs for gopacket.SerializableLayer for more info.
func (ip6 *IPv6) SerializeTo(b gopacket.SerializeBuffer, opts gopacket.SerializeOptions) error {
	var jumbo bool
	var err error

	payload := b.Bytes()
	pLen := len(payload)
	if pLen > ipv6MaxPayloadLength {
		jumbo = true
		if opts.FixLengths {
			// We need to set the length later because the hop-by-hop header may
			// not exist or else need padding, so pLen may yet change
			addIPv6JumboOption(ip6)
		} else if ip6.HopByHop == nil {
			return fmt.Errorf("Cannot fit payload length of %d into IPv6 packet", pLen)
		} else {
			_, ok, err := getIPv6HopByHopJumboLength(ip6.HopByHop)
			if err != nil {
				return err
			}
			if !ok {
				return fmt.Errorf("Missing jumbo length hop-by-hop option")
			}
		}
	}
	if ip6.HopByHop != nil {
		if ip6.NextHeader != IPProtocolIPv6HopByHop {
			// Just fix it instead of throwing an error
			ip6.NextHeader = IPProtocolIPv6HopByHop
		}
		err = ip6.HopByHop.SerializeTo(b, opts)
		if err != nil {
			return err
		}
		payload = b.Bytes()
		pLen = len(payload)
		if opts.FixLengths && jumbo {
			err := setIPv6PayloadJumboLength(payload)
			if err != nil {
				return err
			}
		}
	}
	if !jumbo && pLen > ipv6MaxPayloadLength {
		return fmt.Errorf("Cannot fit payload into IPv6 header")
	}
	bytes, err := b.PrependBytes(40)
	if err != nil {
		return err
	}
	bytes[0] = (ip6.Version << 4) | (ip6.TrafficClass >> 4)
	bytes[1] = (ip6.TrafficClass << 4) | uint8(ip6.FlowLabel>>16)
	binary.BigEndian.PutUint16(bytes[2:], uint16(ip6.FlowLabel))
	if opts.FixLengths {
		if jumbo {
			ip6.Length = 0
		} else {
			ip6.Length = uint16(pLen)
		}
	}
	binary.BigEndian.PutUint16(bytes[4:], ip6.Length)
	bytes[6] = byte(ip6.NextHeader)
	bytes[7] = byte(ip6.HopLimit)
	if err := ip6.AddressTo16(); err != nil {
		return err
	}
	copy(bytes[8:], ip6.SrcIP)
	copy(bytes[24:], ip6.DstIP)
	return nil
}

func (ip6 *IPv6) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	ip6.Version = uint8(data[0]) >> 4
	ip6.TrafficClass = uint8((binary.BigEndian.Uint16(data[0:2]) >> 4) & 0x00FF)
	ip6.FlowLabel = binary.BigEndian.Uint32(data[0:4]) & 0x000FFFFF
	ip6.Length = binary.BigEndian.Uint16(data[4:6])
	ip6.NextHeader = IPProtocol(data[6])
	ip6.HopLimit = data[7]
	ip6.SrcIP = data[8:24]
	ip6.DstIP = data[24:40]
	ip6.HopByHop = nil
	ip6.BaseLayer = BaseLayer{data[:40], data[40:]}

	// We treat a HopByHop IPv6 option as part of the IPv6 packet, since its
	// options are crucial for understanding what's actually happening per packet.
	if ip6.NextHeader == IPProtocolIPv6HopByHop {
		err := ip6.hbh.DecodeFromBytes(ip6.Payload, df)
		if err != nil {
			return err
		}
		ip6.HopByHop = &ip6.hbh
		pEnd, jumbo, err := getIPv6HopByHopJumboLength(ip6.HopByHop)
		if err != nil {
			return err
		}
		if jumbo && ip6.Length == 0 {
			pEnd := int(pEnd)
			if pEnd > len(ip6.Payload) {
				df.SetTruncated()
				pEnd = len(ip6.Payload)
			}
			ip6.Payload = ip6.Payload[:pEnd]
			return nil
		} else if jumbo && ip6.Length != 0 {
			return fmt.Errorf("IPv6 has jumbo length and IPv6 length is not 0")
		} else if !jumbo && ip6.Length == 0 {
			return fmt.Errorf("IPv6 length 0, but HopByHop header does not have jumbogram option")
		}
	}

	if ip6.Length == 0 {
		return fmt.Errorf("IPv6 length 0, but next header is %v, not HopByHop", ip6.NextHeader)
	} else {
		pEnd := int(ip6.Length)
		if pEnd > len(ip6.Payload) {
			df.SetTruncated()
			pEnd = len(ip6.Payload)
		}
		ip6.Payload = ip6.Payload[:pEnd]
	}
	return nil
}

func (i *IPv6) CanDecode() gopacket.LayerClass {
	return LayerTypeIPv6
}

func (i *IPv6) NextLayerType() gopacket.LayerType {
	if i.HopByHop != nil {
		return i.HopByHop.NextHeader.LayerType()
	}
	return i.NextHeader.LayerType()
}

func decodeIPv6(data []byte, p gopacket.PacketBuilder) error {
	ip6 := &IPv6{}
	err := ip6.DecodeFromBytes(data, p)
	p.AddLayer(ip6)
	p.SetNetworkLayer(ip6)
	if ip6.HopByHop != nil {
		p.AddLayer(ip6.HopByHop)
	}
	if err != nil {
		return err
	}
	if ip6.HopByHop != nil {
		return p.NextDecoder(ip6.HopByHop.NextHeader)
	}
	return p.NextDecoder(ip6.NextHeader)
}

type ipv6HeaderTLVOption struct {
	OptionType, OptionLength uint8
	ActualLength             int
	OptionData               []byte
	OptionAlignment          [2]uint8 // Xn+Y = [2]uint8{X, Y}
}

func (h *ipv6HeaderTLVOption) serializeTo(data []byte, fixLengths bool, dryrun bool) int {
	if fixLengths {
		h.OptionLength = uint8(len(h.OptionData))
	}
	length := int(h.OptionLength) + 2
	if !dryrun {
		data[0] = h.OptionType
		data[1] = h.OptionLength
		copy(data[2:], h.OptionData)
	}
	return length
}

func decodeIPv6HeaderTLVOption(data []byte) (h *ipv6HeaderTLVOption) {
	h = &ipv6HeaderTLVOption{}
	if data[0] == 0 {
		h.ActualLength = 1
		return
	}
	h.OptionType = data[0]
	h.OptionLength = data[1]
	h.ActualLength = int(h.OptionLength) + 2
	h.OptionData = data[2:h.ActualLength]
	return
}

func serializeTLVOptionPadding(data []byte, padLength int) {
	if padLength <= 0 {
		return
	}
	if padLength == 1 {
		data[0] = 0x0
		return
	}
	tlvLength := uint8(padLength) - 2
	data[0] = 0x1
	data[1] = tlvLength
	if tlvLength != 0 {
		for k := range data[2:] {
			data[k+2] = 0x0
		}
	}
	return
}

// If buf is 'nil' do a serialize dry run
func serializeIPv6HeaderTLVOptions(buf []byte, options []*ipv6HeaderTLVOption, fixLengths bool) int {
	var l int

	dryrun := buf == nil
	length := 2
	for _, opt := range options {
		if fixLengths {
			x := int(opt.OptionAlignment[0])
			y := int(opt.OptionAlignment[1])
			if x != 0 {
				n := length / x
				offset := x*n + y
				if offset < length {
					offset += x
				}
				if length != offset {
					pad := offset - length
					if !dryrun {
						serializeTLVOptionPadding(buf[length-2:], pad)
					}
					length += pad
				}
			}
		}
		if dryrun {
			l = opt.serializeTo(nil, fixLengths, true)
		} else {
			l = opt.serializeTo(buf[length-2:], fixLengths, false)
		}
		length += l
	}
	if fixLengths {
		pad := length % 8
		if pad != 0 {
			if !dryrun {
				serializeTLVOptionPadding(buf[length-2:], pad)
			}
			length += pad
		}
	}
	return length - 2
}

type ipv6ExtensionBase struct {
	BaseLayer
	NextHeader   IPProtocol
	HeaderLength uint8
	ActualLength int
}

func decodeIPv6ExtensionBase(data []byte) (i ipv6ExtensionBase) {
	i.NextHeader = IPProtocol(data[0])
	i.HeaderLength = data[1]
	i.ActualLength = int(i.HeaderLength)*8 + 8
	i.Contents = data[:i.ActualLength]
	i.Payload = data[i.ActualLength:]
	return
}

// IPv6ExtensionSkipper is a DecodingLayer which decodes and ignores v6
// extensions.  You can use it with a DecodingLayerParser to handle IPv6 stacks
// which may or may not have extensions.
type IPv6ExtensionSkipper struct {
	NextHeader IPProtocol
	BaseLayer
}

func (i *IPv6ExtensionSkipper) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	extension := decodeIPv6ExtensionBase(data)
	i.BaseLayer = BaseLayer{data[:extension.ActualLength], data[extension.ActualLength:]}
	i.NextHeader = extension.NextHeader
	return nil
}

func (i *IPv6ExtensionSkipper) CanDecode() gopacket.LayerClass {
	return LayerClassIPv6Extension
}

func (i *IPv6ExtensionSkipper) NextLayerType() gopacket.LayerType {
	return i.NextHeader.LayerType()
}

// IPv6HopByHopOption is a TLV option present in an IPv6 hop-by-hop extension.
type IPv6HopByHopOption ipv6HeaderTLVOption

// IPv6HopByHop is the IPv6 hop-by-hop extension.
type IPv6HopByHop struct {
	ipv6ExtensionBase
	Options []*IPv6HopByHopOption
}

// LayerType returns LayerTypeIPv6HopByHop.
func (i *IPv6HopByHop) LayerType() gopacket.LayerType { return LayerTypeIPv6HopByHop }

func (i *IPv6HopByHop) SerializeTo(b gopacket.SerializeBuffer, opts gopacket.SerializeOptions) error {
	var bytes []byte
	var err error

	o := make([]*ipv6HeaderTLVOption, 0, len(i.Options))
	for _, v := range i.Options {
		o = append(o, (*ipv6HeaderTLVOption)(v))
	}

	l := serializeIPv6HeaderTLVOptions(nil, o, opts.FixLengths)
	bytes, err = b.PrependBytes(l)
	if err != nil {
		return err
	}
	serializeIPv6HeaderTLVOptions(bytes, o, opts.FixLengths)

	length := len(bytes) + 2
	if length%8 != 0 {
		return fmt.Errorf("IPv6HopByHop actual length must be multiple of 8")
	}
	bytes, err = b.PrependBytes(2)
	if err != nil {
		return err
	}
	bytes[0] = uint8(i.NextHeader)
	if opts.FixLengths {
		i.HeaderLength = uint8((length / 8) - 1)
	}
	bytes[1] = uint8(i.HeaderLength)
	return nil
}

func (i *IPv6HopByHop) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	i.ipv6ExtensionBase = decodeIPv6ExtensionBase(data)
	offset := 2
	for offset < i.ActualLength {
		opt := decodeIPv6HeaderTLVOption(data[offset:])
		i.Options = append(i.Options, (*IPv6HopByHopOption)(opt))
		offset += opt.ActualLength
	}
	return nil
}

func decodeIPv6HopByHop(data []byte, p gopacket.PacketBuilder) error {
	i := &IPv6HopByHop{}
	err := i.DecodeFromBytes(data, p)
	p.AddLayer(i)
	if err != nil {
		return err
	}
	return p.NextDecoder(i.NextHeader)
}

func (o *IPv6HopByHopOption) SetJumboLength(len uint32) {
	o.OptionType = IPv6HopByHopOptionJumbogram
	o.OptionLength = 4
	o.ActualLength = 6
	if o.OptionData == nil {
		o.OptionData = make([]byte, 4)
	}
	binary.BigEndian.PutUint32(o.OptionData, len)
	o.OptionAlignment = [2]uint8{4, 2}
}

// IPv6Routing is the IPv6 routing extension.
type IPv6Routing struct {
	ipv6ExtensionBase
	RoutingType  uint8
	SegmentsLeft uint8
	// This segment is supposed to be zero according to RFC2460, the second set of
	// 4 bytes in the extension.
	Reserved []byte
	// SourceRoutingIPs is the set of IPv6 addresses requested for source routing,
	// set only if RoutingType == 0.
	SourceRoutingIPs []net.IP
}

// LayerType returns LayerTypeIPv6Routing.
func (i *IPv6Routing) LayerType() gopacket.LayerType { return LayerTypeIPv6Routing }

func decodeIPv6Routing(data []byte, p gopacket.PacketBuilder) error {
	i := &IPv6Routing{
		ipv6ExtensionBase: decodeIPv6ExtensionBase(data),
		RoutingType:       data[2],
		SegmentsLeft:      data[3],
		Reserved:          data[4:8],
	}
	switch i.RoutingType {
	case 0: // Source routing
		if (i.ActualLength-8)%16 != 0 {
			return fmt.Errorf("Invalid IPv6 source routing, length of type 0 packet %d", i.ActualLength)
		}
		for d := i.Contents[8:]; len(d) >= 16; d = d[16:] {
			i.SourceRoutingIPs = append(i.SourceRoutingIPs, net.IP(d[:16]))
		}
	default:
		return fmt.Errorf("Unknown IPv6 routing header type %d", i.RoutingType)
	}
	p.AddLayer(i)
	return p.NextDecoder(i.NextHeader)
}

// IPv6Fragment is the IPv6 fragment header, used for packet
// fragmentation/defragmentation.
type IPv6Fragment struct {
	BaseLayer
	NextHeader IPProtocol
	// Reserved1 is bits [8-16), from least to most significant, 0-indexed
	Reserved1      uint8
	FragmentOffset uint16
	// Reserved2 is bits [29-31), from least to most significant, 0-indexed
	Reserved2      uint8
	MoreFragments  bool
	Identification uint32
}

// LayerType returns LayerTypeIPv6Fragment.
func (i *IPv6Fragment) LayerType() gopacket.LayerType { return LayerTypeIPv6Fragment }

func decodeIPv6Fragment(data []byte, p gopacket.PacketBuilder) error {
	i := &IPv6Fragment{
		BaseLayer:      BaseLayer{data[:8], data[8:]},
		NextHeader:     IPProtocol(data[0]),
		Reserved1:      data[1],
		FragmentOffset: binary.BigEndian.Uint16(data[2:4]) >> 3,
		Reserved2:      data[3] & 0x6 >> 1,
		MoreFragments:  data[3]&0x1 != 0,
		Identification: binary.BigEndian.Uint32(data[4:8]),
	}
	p.AddLayer(i)
	return p.NextDecoder(gopacket.DecodeFragment)
}

// IPv6DestinationOption is a TLV option present in an IPv6 destination options extension.
type IPv6DestinationOption ipv6HeaderTLVOption

// IPv6Destination is the IPv6 destination options header.
type IPv6Destination struct {
	ipv6ExtensionBase
	Options []*IPv6DestinationOption
}

// LayerType returns LayerTypeIPv6Destination.
func (i *IPv6Destination) LayerType() gopacket.LayerType { return LayerTypeIPv6Destination }

func (i *IPv6Destination) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	i.ipv6ExtensionBase = decodeIPv6ExtensionBase(data)
	offset := 2
	for offset < i.ActualLength {
		opt := decodeIPv6HeaderTLVOption(data[offset:])
		i.Options = append(i.Options, (*IPv6DestinationOption)(opt))
		offset += opt.ActualLength
	}
	return nil
}

func decodeIPv6Destination(data []byte, p gopacket.PacketBuilder) error {
	i := &IPv6Destination{}
	err := i.DecodeFromBytes(data, p)
	p.AddLayer(i)
	if err != nil {
		return err
	}
	return p.NextDecoder(i.NextHeader)
}

// SerializeTo writes the serialized form of this layer into the
// SerializationBuffer, implementing gopacket.SerializableLayer.
// See the docs for gopacket.SerializableLayer for more info.
func (i *IPv6Destination) SerializeTo(b gopacket.SerializeBuffer, opts gopacket.SerializeOptions) error {
	var bytes []byte
	var err error

	o := make([]*ipv6HeaderTLVOption, 0, len(i.Options))
	for _, v := range i.Options {
		o = append(o, (*ipv6HeaderTLVOption)(v))
	}

	l := serializeIPv6HeaderTLVOptions(nil, o, opts.FixLengths)
	bytes, err = b.PrependBytes(l)
	if err != nil {
		return err
	}
	serializeIPv6HeaderTLVOptions(bytes, o, opts.FixLengths)

	length := len(bytes) + 2
	if length%8 != 0 {
		return fmt.Errorf("IPv6Destination actual length must be multiple of 8")
	}
	bytes, err = b.PrependBytes(2)
	if err != nil {
		return err
	}
	bytes[0] = uint8(i.NextHeader)
	if opts.FixLengths {
		i.HeaderLength = uint8((length / 8) - 1)
	}
	bytes[1] = uint8(i.HeaderLength)
	return nil
}

func checkIPv6Address(addr net.IP) error {
	if len(addr) == net.IPv6len {
		return nil
	}
	if len(addr) == net.IPv4len {
		return fmt.Errorf("address is IPv4")
	}
	return fmt.Errorf("wrong length of %d bytes instead of %d", len(addr), net.IPv6len)
}

func (ip *IPv6) AddressTo16() error {
	if err := checkIPv6Address(ip.SrcIP); err != nil {
		return fmt.Errorf("Invalid source IPv6 address (%s)", err)
	}
	if err := checkIPv6Address(ip.DstIP); err != nil {
		return fmt.Errorf("Invalid destination IPv6 address (%s)", err)
	}
	return nil
}
