// Copyright 2014 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// See http://standards.ieee.org/findstds/standard/802.11-2012.html for info on
// all of the layers in this file.

package layers

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"github.com/google/gopacket"
	"hash/crc32"
	"net"
)

// Dot11Flags contains the set of 8 flags in the IEEE 802.11 frame control
// header, all in one place.
type Dot11Flags uint8

const (
	Dot11FlagsToDS Dot11Flags = 1 << iota
	Dot11FlagsFromDS
	Dot11FlagsMF
	Dot11FlagsRetry
	Dot11FlagsPowerManagement
	Dot11FlagsMD
	Dot11FlagsWEP
	Dot11FlagsOrder
)

func (d Dot11Flags) ToDS() bool {
	return d&Dot11FlagsToDS != 0
}
func (d Dot11Flags) FromDS() bool {
	return d&Dot11FlagsFromDS != 0
}
func (d Dot11Flags) MF() bool {
	return d&Dot11FlagsMF != 0
}
func (d Dot11Flags) Retry() bool {
	return d&Dot11FlagsRetry != 0
}
func (d Dot11Flags) PowerManagement() bool {
	return d&Dot11FlagsPowerManagement != 0
}
func (d Dot11Flags) MD() bool {
	return d&Dot11FlagsMD != 0
}
func (d Dot11Flags) WEP() bool {
	return d&Dot11FlagsWEP != 0
}
func (d Dot11Flags) Order() bool {
	return d&Dot11FlagsOrder != 0
}

// String provides a human readable string for Dot11Flags.
// This string is possibly subject to change over time; if you're storing this
// persistently, you should probably store the Dot11Flags value, not its string.
func (a Dot11Flags) String() string {
	var out bytes.Buffer
	if a.ToDS() {
		out.WriteString("TO-DS,")
	}
	if a.FromDS() {
		out.WriteString("FROM-DS,")
	}
	if a.MF() {
		out.WriteString("MF,")
	}
	if a.Retry() {
		out.WriteString("Retry,")
	}
	if a.PowerManagement() {
		out.WriteString("PowerManagement,")
	}
	if a.MD() {
		out.WriteString("MD,")
	}
	if a.WEP() {
		out.WriteString("WEP,")
	}
	if a.Order() {
		out.WriteString("Order,")
	}

	if length := out.Len(); length > 0 {
		return string(out.Bytes()[:length-1]) // strip final comma
	}
	return ""
}

type Dot11Reason uint16

// TODO: Verify these reasons, and append more reasons if necessary.

const (
	Dot11ReasonReserved          Dot11Reason = 1
	Dot11ReasonUnspecified       Dot11Reason = 2
	Dot11ReasonAuthExpired       Dot11Reason = 3
	Dot11ReasonDeauthStLeaving   Dot11Reason = 4
	Dot11ReasonInactivity        Dot11Reason = 5
	Dot11ReasonApFull            Dot11Reason = 6
	Dot11ReasonClass2FromNonAuth Dot11Reason = 7
	Dot11ReasonClass3FromNonAss  Dot11Reason = 8
	Dot11ReasonDisasStLeaving    Dot11Reason = 9
	Dot11ReasonStNotAuth         Dot11Reason = 10
)

// String provides a human readable string for Dot11Reason.
// This string is possibly subject to change over time; if you're storing this
// persistently, you should probably store the Dot11Reason value, not its string.
func (a Dot11Reason) String() string {
	switch a {
	case Dot11ReasonReserved:
		return "Reserved"
	case Dot11ReasonUnspecified:
		return "Unspecified"
	case Dot11ReasonAuthExpired:
		return "Auth. expired"
	case Dot11ReasonDeauthStLeaving:
		return "Deauth. st. leaving"
	case Dot11ReasonInactivity:
		return "Inactivity"
	case Dot11ReasonApFull:
		return "Ap. full"
	case Dot11ReasonClass2FromNonAuth:
		return "Class2 from non auth."
	case Dot11ReasonClass3FromNonAss:
		return "Class3 from non ass."
	case Dot11ReasonDisasStLeaving:
		return "Disass st. leaving"
	case Dot11ReasonStNotAuth:
		return "St. not auth."
	default:
		return "Unknown reason"
	}
}

type Dot11Status uint16

const (
	Dot11StatusSuccess                      Dot11Status = 0
	Dot11StatusFailure                      Dot11Status = 1  // Unspecified failure
	Dot11StatusCannotSupportAllCapabilities Dot11Status = 10 // Cannot support all requested capabilities in the Capability Information field
	Dot11StatusInabilityExistsAssociation   Dot11Status = 11 // Reassociation denied due to inability to confirm that association exists
	Dot11StatusAssociationDenied            Dot11Status = 12 // Association denied due to reason outside the scope of this standard
	Dot11StatusAlgorithmUnsupported         Dot11Status = 13 // Responding station does not support the specified authentication algorithm
	Dot11StatusOufOfExpectedSequence        Dot11Status = 14 // Received an Authentication frame with authentication transaction sequence number out of expected sequence
	Dot11StatusChallengeFailure             Dot11Status = 15 // Authentication rejected because of challenge failure
	Dot11StatusTimeout                      Dot11Status = 16 // Authentication rejected due to timeout waiting for next frame in sequence
	Dot11StatusAPUnableToHandle             Dot11Status = 17 // Association denied because AP is unable to handle additional associated stations
	Dot11StatusRateUnsupported              Dot11Status = 18 // Association denied due to requesting station not supporting all of the data rates in the BSSBasicRateSet parameter
)

// String provides a human readable string for Dot11Status.
// This string is possibly subject to change over time; if you're storing this
// persistently, you should probably store the Dot11Status value, not its string.
func (a Dot11Status) String() string {
	switch a {
	case Dot11StatusSuccess:
		return "success"
	case Dot11StatusFailure:
		return "failure"
	case Dot11StatusCannotSupportAllCapabilities:
		return "cannot-support-all-capabilities"
	case Dot11StatusInabilityExistsAssociation:
		return "inability-exists-association"
	case Dot11StatusAssociationDenied:
		return "association-denied"
	case Dot11StatusAlgorithmUnsupported:
		return "algorithm-unsupported"
	case Dot11StatusOufOfExpectedSequence:
		return "out-of-expected-sequence"
	case Dot11StatusChallengeFailure:
		return "challenge-failure"
	case Dot11StatusTimeout:
		return "timeout"
	case Dot11StatusAPUnableToHandle:
		return "ap-unable-to-handle"
	case Dot11StatusRateUnsupported:
		return "rate-unsupported"
	default:
		return "unknown status"
	}
}

type Dot11AckPolicy uint8

const (
	Dot11AckPolicyNormal     Dot11AckPolicy = 0
	Dot11AckPolicyNone       Dot11AckPolicy = 1
	Dot11AckPolicyNoExplicit Dot11AckPolicy = 2
	Dot11AckPolicyBlock      Dot11AckPolicy = 3
)

// String provides a human readable string for Dot11AckPolicy.
// This string is possibly subject to change over time; if you're storing this
// persistently, you should probably store the Dot11AckPolicy value, not its string.
func (a Dot11AckPolicy) String() string {
	switch a {
	case Dot11AckPolicyNormal:
		return "normal-ack"
	case Dot11AckPolicyNone:
		return "no-ack"
	case Dot11AckPolicyNoExplicit:
		return "no-explicit-ack"
	case Dot11AckPolicyBlock:
		return "block-ack"
	default:
		return "unknown-ack-policy"
	}
}

type Dot11Algorithm uint16

const (
	Dot11AlgorithmOpen      Dot11Algorithm = 0
	Dot11AlgorithmSharedKey Dot11Algorithm = 1
)

// String provides a human readable string for Dot11Algorithm.
// This string is possibly subject to change over time; if you're storing this
// persistently, you should probably store the Dot11Algorithm value, not its string.
func (a Dot11Algorithm) String() string {
	switch a {
	case Dot11AlgorithmOpen:
		return "open"
	case Dot11AlgorithmSharedKey:
		return "shared-key"
	default:
		return "unknown-algorithm"
	}
}

type Dot11InformationElementID uint8

// TODO: Verify these element ids, and append more ids if more.

const (
	Dot11InformationElementIDSSID          Dot11InformationElementID = 0
	Dot11InformationElementIDRates         Dot11InformationElementID = 1
	Dot11InformationElementIDFHSet         Dot11InformationElementID = 2
	Dot11InformationElementIDDSSet         Dot11InformationElementID = 3
	Dot11InformationElementIDCFSet         Dot11InformationElementID = 4
	Dot11InformationElementIDTIM           Dot11InformationElementID = 5
	Dot11InformationElementIDIBSSSet       Dot11InformationElementID = 6
	Dot11InformationElementIDChallenge     Dot11InformationElementID = 16
	Dot11InformationElementIDERPInfo       Dot11InformationElementID = 42
	Dot11InformationElementIDQOSCapability Dot11InformationElementID = 46
	Dot11InformationElementIDERPInfo2      Dot11InformationElementID = 47
	Dot11InformationElementIDRSNInfo       Dot11InformationElementID = 48
	Dot11InformationElementIDESRates       Dot11InformationElementID = 50
	Dot11InformationElementIDVendor        Dot11InformationElementID = 221
	Dot11InformationElementIDReserved      Dot11InformationElementID = 68
)

// String provides a human readable string for Dot11InformationElementID.
// This string is possibly subject to change over time; if you're storing this
// persistently, you should probably store the Dot11InformationElementID value,
// not its string.
func (a Dot11InformationElementID) String() string {
	switch a {
	case Dot11InformationElementIDSSID:
		return "SSID"
	case Dot11InformationElementIDRates:
		return "Rates"
	case Dot11InformationElementIDFHSet:
		return "FHset"
	case Dot11InformationElementIDDSSet:
		return "DSset"
	case Dot11InformationElementIDCFSet:
		return "CFset"
	case Dot11InformationElementIDTIM:
		return "TIM"
	case Dot11InformationElementIDIBSSSet:
		return "IBSSset"
	case Dot11InformationElementIDChallenge:
		return "Challenge"
	case Dot11InformationElementIDERPInfo:
		return "ERPinfo"
	case Dot11InformationElementIDQOSCapability:
		return "QOS capability"
	case Dot11InformationElementIDERPInfo2:
		return "ERPinfo2"
	case Dot11InformationElementIDRSNInfo:
		return "RSNinfo"
	case Dot11InformationElementIDESRates:
		return "ESrates"
	case Dot11InformationElementIDVendor:
		return "Vendor"
	case Dot11InformationElementIDReserved:
		return "Reserved"
	default:
		return "Unknown information element id"
	}
}

// Dot11 provides an IEEE 802.11 base packet header.
// See http://standards.ieee.org/findstds/standard/802.11-2012.html
// for excrutiating detail.
type Dot11 struct {
	BaseLayer
	Type           Dot11Type
	Proto          uint8
	Flags          Dot11Flags
	DurationID     uint16
	Address1       net.HardwareAddr
	Address2       net.HardwareAddr
	Address3       net.HardwareAddr
	Address4       net.HardwareAddr
	SequenceNumber uint16
	FragmentNumber uint16
	Checksum       uint32
}

func decodeDot11(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11) LayerType() gopacket.LayerType  { return LayerTypeDot11 }
func (m *Dot11) CanDecode() gopacket.LayerClass { return LayerTypeDot11 }
func (m *Dot11) NextLayerType() gopacket.LayerType {
	if m.Flags.WEP() {
		return (LayerTypeDot11WEP)
	}

	return m.Type.LayerType()
}

func (m *Dot11) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 10 {
		df.SetTruncated()
		return fmt.Errorf("Dot11 length %v too short, %v required", len(data), 10)
	}
	m.Type = Dot11Type((data[0])&0xFC) >> 2

	m.Proto = uint8(data[0]) & 0x0003
	m.Flags = Dot11Flags(data[1])
	m.DurationID = binary.LittleEndian.Uint16(data[2:4])
	m.Address1 = net.HardwareAddr(data[4:10])

	offset := 10

	mainType := m.Type.MainType()

	switch mainType {
	case Dot11TypeCtrl:
		switch m.Type {
		case Dot11TypeCtrlRTS, Dot11TypeCtrlPowersavePoll, Dot11TypeCtrlCFEnd, Dot11TypeCtrlCFEndAck:
			if len(data) < offset+6 {
				df.SetTruncated()
				return fmt.Errorf("Dot11 length %v too short, %v required", len(data), offset+6)
			}
			m.Address2 = net.HardwareAddr(data[offset : offset+6])
			offset += 6
		}
	case Dot11TypeMgmt, Dot11TypeData:
		if len(data) < offset+14 {
			df.SetTruncated()
			return fmt.Errorf("Dot11 length %v too short, %v required", len(data), offset+14)
		}
		m.Address2 = net.HardwareAddr(data[offset : offset+6])
		offset += 6
		m.Address3 = net.HardwareAddr(data[offset : offset+6])
		offset += 6

		m.SequenceNumber = (binary.LittleEndian.Uint16(data[offset:offset+2]) & 0xFFF0) >> 4
		m.FragmentNumber = (binary.LittleEndian.Uint16(data[offset:offset+2]) & 0x000F)
		offset += 2
	}

	if mainType == Dot11TypeData && m.Flags.FromDS() && m.Flags.ToDS() {
		if len(data) < offset+6 {
			df.SetTruncated()
			return fmt.Errorf("Dot11 length %v too short, %v required", len(data), offset+6)
		}
		m.Address4 = net.HardwareAddr(data[offset : offset+6])
		offset += 6
	}

	m.BaseLayer = BaseLayer{Contents: data[0:offset], Payload: data[offset : len(data)-4]}
	m.Checksum = binary.LittleEndian.Uint32(data[len(data)-4 : len(data)])
	return nil
}

func (m *Dot11) ChecksumValid() bool {
	// only for CTRL and MGMT frames
	h := crc32.NewIEEE()
	h.Write(m.Contents)
	h.Write(m.Payload)
	return m.Checksum == h.Sum32()
}

// Dot11Mgmt is a base for all IEEE 802.11 management layers.
type Dot11Mgmt struct {
	BaseLayer
}

func (m *Dot11Mgmt) NextLayerType() gopacket.LayerType { return gopacket.LayerTypePayload }
func (m *Dot11Mgmt) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	m.Contents = data
	return nil
}

// Dot11Ctrl is a base for all IEEE 802.11 control layers.
type Dot11Ctrl struct {
	BaseLayer
}

func (m *Dot11Ctrl) NextLayerType() gopacket.LayerType { return gopacket.LayerTypePayload }

func (m *Dot11Ctrl) LayerType() gopacket.LayerType  { return LayerTypeDot11Ctrl }
func (m *Dot11Ctrl) CanDecode() gopacket.LayerClass { return LayerTypeDot11Ctrl }
func (m *Dot11Ctrl) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	m.Contents = data
	return nil
}

func decodeDot11Ctrl(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11Ctrl{}
	return decodingLayerDecoder(d, data, p)
}

// Dot11WEP contains WEP encrpted IEEE 802.11 data.
type Dot11WEP struct {
	BaseLayer
}

func (m *Dot11WEP) NextLayerType() gopacket.LayerType { return LayerTypeLLC }

func (m *Dot11WEP) LayerType() gopacket.LayerType  { return LayerTypeDot11WEP }
func (m *Dot11WEP) CanDecode() gopacket.LayerClass { return LayerTypeDot11WEP }
func (m *Dot11WEP) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	m.Contents = data
	return nil
}

func decodeDot11WEP(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11WEP{}
	return decodingLayerDecoder(d, data, p)
}

// Dot11Data is a base for all IEEE 802.11 data layers.
type Dot11Data struct {
	BaseLayer
}

func (m *Dot11Data) NextLayerType() gopacket.LayerType { return LayerTypeLLC }

func (m *Dot11Data) LayerType() gopacket.LayerType  { return LayerTypeDot11Data }
func (m *Dot11Data) CanDecode() gopacket.LayerClass { return LayerTypeDot11Data }
func (m *Dot11Data) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	m.Payload = data
	return nil
}

func decodeDot11Data(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11Data{}
	return decodingLayerDecoder(d, data, p)
}

type Dot11DataCFAck struct {
	Dot11Data
}

func decodeDot11DataCFAck(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataCFAck{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataCFAck) LayerType() gopacket.LayerType  { return LayerTypeDot11DataCFAck }
func (m *Dot11DataCFAck) CanDecode() gopacket.LayerClass { return LayerTypeDot11DataCFAck }
func (m *Dot11DataCFAck) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Data.DecodeFromBytes(data, df)
}

type Dot11DataCFPoll struct {
	Dot11Data
}

func decodeDot11DataCFPoll(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataCFPoll{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataCFPoll) LayerType() gopacket.LayerType  { return LayerTypeDot11DataCFPoll }
func (m *Dot11DataCFPoll) CanDecode() gopacket.LayerClass { return LayerTypeDot11DataCFPoll }
func (m *Dot11DataCFPoll) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Data.DecodeFromBytes(data, df)
}

type Dot11DataCFAckPoll struct {
	Dot11Data
}

func decodeDot11DataCFAckPoll(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataCFAckPoll{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataCFAckPoll) LayerType() gopacket.LayerType  { return LayerTypeDot11DataCFAckPoll }
func (m *Dot11DataCFAckPoll) CanDecode() gopacket.LayerClass { return LayerTypeDot11DataCFAckPoll }
func (m *Dot11DataCFAckPoll) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Data.DecodeFromBytes(data, df)
}

type Dot11DataNull struct {
	Dot11Data
}

func decodeDot11DataNull(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataNull{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataNull) LayerType() gopacket.LayerType  { return LayerTypeDot11DataNull }
func (m *Dot11DataNull) CanDecode() gopacket.LayerClass { return LayerTypeDot11DataNull }
func (m *Dot11DataNull) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Data.DecodeFromBytes(data, df)
}

type Dot11DataCFAckNoData struct {
	Dot11Data
}

func decodeDot11DataCFAckNoData(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataCFAckNoData{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataCFAckNoData) LayerType() gopacket.LayerType  { return LayerTypeDot11DataCFAckNoData }
func (m *Dot11DataCFAckNoData) CanDecode() gopacket.LayerClass { return LayerTypeDot11DataCFAckNoData }
func (m *Dot11DataCFAckNoData) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Data.DecodeFromBytes(data, df)
}

type Dot11DataCFPollNoData struct {
	Dot11Data
}

func decodeDot11DataCFPollNoData(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataCFPollNoData{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataCFPollNoData) LayerType() gopacket.LayerType  { return LayerTypeDot11DataCFPollNoData }
func (m *Dot11DataCFPollNoData) CanDecode() gopacket.LayerClass { return LayerTypeDot11DataCFPollNoData }
func (m *Dot11DataCFPollNoData) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Data.DecodeFromBytes(data, df)
}

type Dot11DataCFAckPollNoData struct {
	Dot11Data
}

func decodeDot11DataCFAckPollNoData(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataCFAckPollNoData{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataCFAckPollNoData) LayerType() gopacket.LayerType {
	return LayerTypeDot11DataCFAckPollNoData
}
func (m *Dot11DataCFAckPollNoData) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11DataCFAckPollNoData
}
func (m *Dot11DataCFAckPollNoData) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Data.DecodeFromBytes(data, df)
}

type Dot11DataQOS struct {
	Dot11Ctrl
	TID       uint8 /* Traffic IDentifier */
	EOSP      bool  /* End of service period */
	AckPolicy Dot11AckPolicy
	TXOP      uint8
}

func (m *Dot11DataQOS) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 4 {
		df.SetTruncated()
		return fmt.Errorf("Dot11DataQOS length %v too short, %v required", len(data), 4)
	}
	m.TID = (uint8(data[0]) & 0x0F)
	m.EOSP = (uint8(data[0]) & 0x10) == 0x10
	m.AckPolicy = Dot11AckPolicy((uint8(data[0]) & 0x60) >> 5)
	m.TXOP = uint8(data[1])
	// TODO: Mesh Control bytes 2:4
	m.BaseLayer = BaseLayer{Contents: data[0:4], Payload: data[4:]}
	return nil
}

type Dot11DataQOSData struct {
	Dot11DataQOS
}

func decodeDot11DataQOSData(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataQOSData{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataQOSData) LayerType() gopacket.LayerType  { return LayerTypeDot11DataQOSData }
func (m *Dot11DataQOSData) CanDecode() gopacket.LayerClass { return LayerTypeDot11DataQOSData }

func (m *Dot11DataQOSData) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11Data
}

type Dot11DataQOSDataCFAck struct {
	Dot11DataQOS
}

func decodeDot11DataQOSDataCFAck(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataQOSDataCFAck{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataQOSDataCFAck) LayerType() gopacket.LayerType     { return LayerTypeDot11DataQOSDataCFAck }
func (m *Dot11DataQOSDataCFAck) CanDecode() gopacket.LayerClass    { return LayerTypeDot11DataQOSDataCFAck }
func (m *Dot11DataQOSDataCFAck) NextLayerType() gopacket.LayerType { return LayerTypeDot11DataCFAck }

type Dot11DataQOSDataCFPoll struct {
	Dot11DataQOS
}

func decodeDot11DataQOSDataCFPoll(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataQOSDataCFPoll{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataQOSDataCFPoll) LayerType() gopacket.LayerType {
	return LayerTypeDot11DataQOSDataCFPoll
}
func (m *Dot11DataQOSDataCFPoll) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11DataQOSDataCFPoll
}
func (m *Dot11DataQOSDataCFPoll) NextLayerType() gopacket.LayerType { return LayerTypeDot11DataCFPoll }

type Dot11DataQOSDataCFAckPoll struct {
	Dot11DataQOS
}

func decodeDot11DataQOSDataCFAckPoll(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataQOSDataCFAckPoll{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataQOSDataCFAckPoll) LayerType() gopacket.LayerType {
	return LayerTypeDot11DataQOSDataCFAckPoll
}
func (m *Dot11DataQOSDataCFAckPoll) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11DataQOSDataCFAckPoll
}
func (m *Dot11DataQOSDataCFAckPoll) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11DataCFAckPoll
}

type Dot11DataQOSNull struct {
	Dot11DataQOS
}

func decodeDot11DataQOSNull(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataQOSNull{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataQOSNull) LayerType() gopacket.LayerType     { return LayerTypeDot11DataQOSNull }
func (m *Dot11DataQOSNull) CanDecode() gopacket.LayerClass    { return LayerTypeDot11DataQOSNull }
func (m *Dot11DataQOSNull) NextLayerType() gopacket.LayerType { return LayerTypeDot11DataNull }

type Dot11DataQOSCFPollNoData struct {
	Dot11DataQOS
}

func decodeDot11DataQOSCFPollNoData(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataQOSCFPollNoData{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataQOSCFPollNoData) LayerType() gopacket.LayerType {
	return LayerTypeDot11DataQOSCFPollNoData
}
func (m *Dot11DataQOSCFPollNoData) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11DataQOSCFPollNoData
}
func (m *Dot11DataQOSCFPollNoData) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11DataCFPollNoData
}

type Dot11DataQOSCFAckPollNoData struct {
	Dot11DataQOS
}

func decodeDot11DataQOSCFAckPollNoData(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11DataQOSCFAckPollNoData{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11DataQOSCFAckPollNoData) LayerType() gopacket.LayerType {
	return LayerTypeDot11DataQOSCFAckPollNoData
}
func (m *Dot11DataQOSCFAckPollNoData) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11DataQOSCFAckPollNoData
}
func (m *Dot11DataQOSCFAckPollNoData) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11DataCFAckPollNoData
}

type Dot11InformationElement struct {
	BaseLayer
	ID     Dot11InformationElementID
	Length uint8
	OUI    []byte
	Info   []byte
}

func (m *Dot11InformationElement) LayerType() gopacket.LayerType {
	return LayerTypeDot11InformationElement
}
func (m *Dot11InformationElement) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11InformationElement
}

func (m *Dot11InformationElement) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11InformationElement
}

func (m *Dot11InformationElement) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 2 {
		df.SetTruncated()
		return fmt.Errorf("Dot11InformationElement length %v too short, %v required", len(data), 2)
	}
	m.ID = Dot11InformationElementID(data[0])
	m.Length = data[1]
	offset := int(2)

	if len(data) < offset+int(m.Length) {
		df.SetTruncated()
		return fmt.Errorf("Dot11InformationElement length %v too short, %v required", len(data), offset+int(m.Length))
	}
	if m.ID == 221 {
		// Vendor extension
		m.OUI = data[offset : offset+4]
		m.Info = data[offset+4 : offset+int(m.Length)]
	} else {
		m.Info = data[offset : offset+int(m.Length)]
	}

	offset += int(m.Length)

	m.BaseLayer = BaseLayer{Contents: data[:offset], Payload: data[offset:]}
	return nil
}

func (d *Dot11InformationElement) String() string {
	if d.ID == 0 {
		return fmt.Sprintf("802.11 Information Element (SSID: %v)", string(d.Info))
	} else if d.ID == 1 {
		rates := ""
		for i := 0; i < len(d.Info); i++ {
			if d.Info[i]&0x80 == 0 {
				rates += fmt.Sprintf("%.1f ", float32(d.Info[i])*0.5)
			} else {
				rates += fmt.Sprintf("%.1f* ", float32(d.Info[i]&0x7F)*0.5)
			}
		}
		return fmt.Sprintf("802.11 Information Element (Rates: %s Mbit)", rates)
	} else if d.ID == 221 {
		return fmt.Sprintf("802.11 Information Element (Vendor: ID: %v, Length: %v, OUI: %X, Info: %X)", d.ID, d.Length, d.OUI, d.Info)
	} else {
		return fmt.Sprintf("802.11 Information Element (ID: %v, Length: %v, Info: %X)", d.ID, d.Length, d.Info)
	}
}

func (m Dot11InformationElement) SerializeTo(b gopacket.SerializeBuffer, opts gopacket.SerializeOptions) error {
	length := len(m.Info) + len(m.OUI)
	if buf, err := b.PrependBytes(2 + length); err != nil {
		return err
	} else {
		buf[0] = uint8(m.ID)
		buf[1] = uint8(length)
		copy(buf[2:], m.OUI)
		copy(buf[2+len(m.OUI):], m.Info)
	}
	return nil
}

func decodeDot11InformationElement(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11InformationElement{}
	return decodingLayerDecoder(d, data, p)
}

type Dot11CtrlCTS struct {
	Dot11Ctrl
}

func decodeDot11CtrlCTS(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11CtrlCTS{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11CtrlCTS) LayerType() gopacket.LayerType {
	return LayerTypeDot11CtrlCTS
}
func (m *Dot11CtrlCTS) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11CtrlCTS
}
func (m *Dot11CtrlCTS) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Ctrl.DecodeFromBytes(data, df)
}

type Dot11CtrlRTS struct {
	Dot11Ctrl
}

func decodeDot11CtrlRTS(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11CtrlRTS{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11CtrlRTS) LayerType() gopacket.LayerType {
	return LayerTypeDot11CtrlRTS
}
func (m *Dot11CtrlRTS) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11CtrlRTS
}
func (m *Dot11CtrlRTS) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Ctrl.DecodeFromBytes(data, df)
}

type Dot11CtrlBlockAckReq struct {
	Dot11Ctrl
}

func decodeDot11CtrlBlockAckReq(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11CtrlBlockAckReq{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11CtrlBlockAckReq) LayerType() gopacket.LayerType {
	return LayerTypeDot11CtrlBlockAckReq
}
func (m *Dot11CtrlBlockAckReq) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11CtrlBlockAckReq
}
func (m *Dot11CtrlBlockAckReq) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Ctrl.DecodeFromBytes(data, df)
}

type Dot11CtrlBlockAck struct {
	Dot11Ctrl
}

func decodeDot11CtrlBlockAck(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11CtrlBlockAck{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11CtrlBlockAck) LayerType() gopacket.LayerType  { return LayerTypeDot11CtrlBlockAck }
func (m *Dot11CtrlBlockAck) CanDecode() gopacket.LayerClass { return LayerTypeDot11CtrlBlockAck }
func (m *Dot11CtrlBlockAck) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Ctrl.DecodeFromBytes(data, df)
}

type Dot11CtrlPowersavePoll struct {
	Dot11Ctrl
}

func decodeDot11CtrlPowersavePoll(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11CtrlPowersavePoll{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11CtrlPowersavePoll) LayerType() gopacket.LayerType {
	return LayerTypeDot11CtrlPowersavePoll
}
func (m *Dot11CtrlPowersavePoll) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11CtrlPowersavePoll
}
func (m *Dot11CtrlPowersavePoll) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Ctrl.DecodeFromBytes(data, df)
}

type Dot11CtrlAck struct {
	Dot11Ctrl
}

func decodeDot11CtrlAck(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11CtrlAck{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11CtrlAck) LayerType() gopacket.LayerType  { return LayerTypeDot11CtrlAck }
func (m *Dot11CtrlAck) CanDecode() gopacket.LayerClass { return LayerTypeDot11CtrlAck }
func (m *Dot11CtrlAck) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Ctrl.DecodeFromBytes(data, df)
}

type Dot11CtrlCFEnd struct {
	Dot11Ctrl
}

func decodeDot11CtrlCFEnd(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11CtrlCFEnd{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11CtrlCFEnd) LayerType() gopacket.LayerType {
	return LayerTypeDot11CtrlCFEnd
}
func (m *Dot11CtrlCFEnd) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11CtrlCFEnd
}
func (m *Dot11CtrlCFEnd) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Ctrl.DecodeFromBytes(data, df)
}

type Dot11CtrlCFEndAck struct {
	Dot11Ctrl
}

func decodeDot11CtrlCFEndAck(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11CtrlCFEndAck{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11CtrlCFEndAck) LayerType() gopacket.LayerType {
	return LayerTypeDot11CtrlCFEndAck
}
func (m *Dot11CtrlCFEndAck) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11CtrlCFEndAck
}
func (m *Dot11CtrlCFEndAck) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	return m.Dot11Ctrl.DecodeFromBytes(data, df)
}

type Dot11MgmtAssociationReq struct {
	Dot11Mgmt
	CapabilityInfo uint16
	ListenInterval uint16
}

func decodeDot11MgmtAssociationReq(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtAssociationReq{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtAssociationReq) LayerType() gopacket.LayerType {
	return LayerTypeDot11MgmtAssociationReq
}
func (m *Dot11MgmtAssociationReq) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11MgmtAssociationReq
}
func (m *Dot11MgmtAssociationReq) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11InformationElement
}
func (m *Dot11MgmtAssociationReq) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 4 {
		df.SetTruncated()
		return fmt.Errorf("Dot11MgmtAssociationReq length %v too short, %v required", len(data), 4)
	}
	m.CapabilityInfo = binary.LittleEndian.Uint16(data[0:2])
	m.ListenInterval = binary.LittleEndian.Uint16(data[2:4])
	m.Payload = data[4:]
	return m.Dot11Mgmt.DecodeFromBytes(data, df)
}

type Dot11MgmtAssociationResp struct {
	Dot11Mgmt
	CapabilityInfo uint16
	Status         Dot11Status
	AID            uint16
}

func decodeDot11MgmtAssociationResp(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtAssociationResp{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtAssociationResp) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11MgmtAssociationResp
}
func (m *Dot11MgmtAssociationResp) LayerType() gopacket.LayerType {
	return LayerTypeDot11MgmtAssociationResp
}
func (m *Dot11MgmtAssociationResp) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11InformationElement
}
func (m *Dot11MgmtAssociationResp) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 6 {
		df.SetTruncated()
		return fmt.Errorf("Dot11MgmtAssociationResp length %v too short, %v required", len(data), 6)
	}
	m.CapabilityInfo = binary.LittleEndian.Uint16(data[0:2])
	m.Status = Dot11Status(binary.LittleEndian.Uint16(data[2:4]))
	m.AID = binary.LittleEndian.Uint16(data[4:6])
	m.Payload = data[6:]
	return m.Dot11Mgmt.DecodeFromBytes(data, df)
}

type Dot11MgmtReassociationReq struct {
	Dot11Mgmt
	CapabilityInfo   uint16
	ListenInterval   uint16
	CurrentApAddress net.HardwareAddr
}

func decodeDot11MgmtReassociationReq(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtReassociationReq{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtReassociationReq) LayerType() gopacket.LayerType {
	return LayerTypeDot11MgmtReassociationReq
}
func (m *Dot11MgmtReassociationReq) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11MgmtReassociationReq
}
func (m *Dot11MgmtReassociationReq) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11InformationElement
}
func (m *Dot11MgmtReassociationReq) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 10 {
		df.SetTruncated()
		return fmt.Errorf("Dot11MgmtReassociationReq length %v too short, %v required", len(data), 10)
	}
	m.CapabilityInfo = binary.LittleEndian.Uint16(data[0:2])
	m.ListenInterval = binary.LittleEndian.Uint16(data[2:4])
	m.CurrentApAddress = net.HardwareAddr(data[4:10])
	m.Payload = data[10:]
	return m.Dot11Mgmt.DecodeFromBytes(data, df)
}

type Dot11MgmtReassociationResp struct {
	Dot11Mgmt
}

func decodeDot11MgmtReassociationResp(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtReassociationResp{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtReassociationResp) LayerType() gopacket.LayerType {
	return LayerTypeDot11MgmtReassociationResp
}
func (m *Dot11MgmtReassociationResp) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11MgmtReassociationResp
}
func (m *Dot11MgmtReassociationResp) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11InformationElement
}

type Dot11MgmtProbeReq struct {
	Dot11Mgmt
}

func decodeDot11MgmtProbeReq(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtProbeReq{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtProbeReq) LayerType() gopacket.LayerType  { return LayerTypeDot11MgmtProbeReq }
func (m *Dot11MgmtProbeReq) CanDecode() gopacket.LayerClass { return LayerTypeDot11MgmtProbeReq }
func (m *Dot11MgmtProbeReq) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11InformationElement
}

type Dot11MgmtProbeResp struct {
	Dot11Mgmt
}

func decodeDot11MgmtProbeResp(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtProbeResp{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtProbeResp) LayerType() gopacket.LayerType  { return LayerTypeDot11MgmtProbeResp }
func (m *Dot11MgmtProbeResp) CanDecode() gopacket.LayerClass { return LayerTypeDot11MgmtProbeResp }
func (m *Dot11MgmtProbeResp) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11InformationElement
}

type Dot11MgmtMeasurementPilot struct {
	Dot11Mgmt
}

func decodeDot11MgmtMeasurementPilot(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtMeasurementPilot{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtMeasurementPilot) LayerType() gopacket.LayerType {
	return LayerTypeDot11MgmtMeasurementPilot
}
func (m *Dot11MgmtMeasurementPilot) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11MgmtMeasurementPilot
}

type Dot11MgmtBeacon struct {
	Dot11Mgmt
	Timestamp uint64
	Interval  uint16
	Flags     uint16
}

func decodeDot11MgmtBeacon(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtBeacon{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtBeacon) LayerType() gopacket.LayerType  { return LayerTypeDot11MgmtBeacon }
func (m *Dot11MgmtBeacon) CanDecode() gopacket.LayerClass { return LayerTypeDot11MgmtBeacon }
func (m *Dot11MgmtBeacon) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 12 {
		df.SetTruncated()
		return fmt.Errorf("Dot11MgmtBeacon length %v too short, %v required", len(data), 12)
	}
	m.Timestamp = binary.LittleEndian.Uint64(data[0:8])
	m.Interval = binary.LittleEndian.Uint16(data[8:10])
	m.Flags = binary.LittleEndian.Uint16(data[10:12])
	m.Payload = data[12:]
	return m.Dot11Mgmt.DecodeFromBytes(data, df)
}

func (m *Dot11MgmtBeacon) NextLayerType() gopacket.LayerType { return LayerTypeDot11InformationElement }

type Dot11MgmtATIM struct {
	Dot11Mgmt
}

func decodeDot11MgmtATIM(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtATIM{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtATIM) LayerType() gopacket.LayerType  { return LayerTypeDot11MgmtATIM }
func (m *Dot11MgmtATIM) CanDecode() gopacket.LayerClass { return LayerTypeDot11MgmtATIM }

type Dot11MgmtDisassociation struct {
	Dot11Mgmt
	Reason Dot11Reason
}

func decodeDot11MgmtDisassociation(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtDisassociation{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtDisassociation) LayerType() gopacket.LayerType {
	return LayerTypeDot11MgmtDisassociation
}
func (m *Dot11MgmtDisassociation) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11MgmtDisassociation
}
func (m *Dot11MgmtDisassociation) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 2 {
		df.SetTruncated()
		return fmt.Errorf("Dot11MgmtDisassociation length %v too short, %v required", len(data), 2)
	}
	m.Reason = Dot11Reason(binary.LittleEndian.Uint16(data[0:2]))
	return m.Dot11Mgmt.DecodeFromBytes(data, df)
}

type Dot11MgmtAuthentication struct {
	Dot11Mgmt
	Algorithm Dot11Algorithm
	Sequence  uint16
	Status    Dot11Status
}

func decodeDot11MgmtAuthentication(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtAuthentication{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtAuthentication) LayerType() gopacket.LayerType {
	return LayerTypeDot11MgmtAuthentication
}
func (m *Dot11MgmtAuthentication) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11MgmtAuthentication
}
func (m *Dot11MgmtAuthentication) NextLayerType() gopacket.LayerType {
	return LayerTypeDot11InformationElement
}
func (m *Dot11MgmtAuthentication) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 6 {
		df.SetTruncated()
		return fmt.Errorf("Dot11MgmtAuthentication length %v too short, %v required", len(data), 6)
	}
	m.Algorithm = Dot11Algorithm(binary.LittleEndian.Uint16(data[0:2]))
	m.Sequence = binary.LittleEndian.Uint16(data[2:4])
	m.Status = Dot11Status(binary.LittleEndian.Uint16(data[4:6]))
	m.Payload = data[6:]
	return m.Dot11Mgmt.DecodeFromBytes(data, df)
}

type Dot11MgmtDeauthentication struct {
	Dot11Mgmt
	Reason Dot11Reason
}

func decodeDot11MgmtDeauthentication(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtDeauthentication{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtDeauthentication) LayerType() gopacket.LayerType {
	return LayerTypeDot11MgmtDeauthentication
}
func (m *Dot11MgmtDeauthentication) CanDecode() gopacket.LayerClass {
	return LayerTypeDot11MgmtDeauthentication
}
func (m *Dot11MgmtDeauthentication) DecodeFromBytes(data []byte, df gopacket.DecodeFeedback) error {
	if len(data) < 2 {
		df.SetTruncated()
		return fmt.Errorf("Dot11MgmtDeauthentication length %v too short, %v required", len(data), 2)
	}
	m.Reason = Dot11Reason(binary.LittleEndian.Uint16(data[0:2]))
	return m.Dot11Mgmt.DecodeFromBytes(data, df)
}

type Dot11MgmtAction struct {
	Dot11Mgmt
}

func decodeDot11MgmtAction(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtAction{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtAction) LayerType() gopacket.LayerType  { return LayerTypeDot11MgmtAction }
func (m *Dot11MgmtAction) CanDecode() gopacket.LayerClass { return LayerTypeDot11MgmtAction }

type Dot11MgmtActionNoAck struct {
	Dot11Mgmt
}

func decodeDot11MgmtActionNoAck(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtActionNoAck{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtActionNoAck) LayerType() gopacket.LayerType  { return LayerTypeDot11MgmtActionNoAck }
func (m *Dot11MgmtActionNoAck) CanDecode() gopacket.LayerClass { return LayerTypeDot11MgmtActionNoAck }

type Dot11MgmtArubaWLAN struct {
	Dot11Mgmt
}

func decodeDot11MgmtArubaWLAN(data []byte, p gopacket.PacketBuilder) error {
	d := &Dot11MgmtArubaWLAN{}
	return decodingLayerDecoder(d, data, p)
}

func (m *Dot11MgmtArubaWLAN) LayerType() gopacket.LayerType  { return LayerTypeDot11MgmtArubaWLAN }
func (m *Dot11MgmtArubaWLAN) CanDecode() gopacket.LayerClass { return LayerTypeDot11MgmtArubaWLAN }
