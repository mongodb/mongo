// Copyright 2012 Google, gopacket.LayerTypeMetadata{Inc. All rights reserved}.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"github.com/google/gopacket"
)

var (
	LayerTypeARP                         = gopacket.RegisterLayerType(10, gopacket.LayerTypeMetadata{"ARP", gopacket.DecodeFunc(decodeARP)})
	LayerTypeCiscoDiscovery              = gopacket.RegisterLayerType(11, gopacket.LayerTypeMetadata{"CiscoDiscovery", gopacket.DecodeFunc(decodeCiscoDiscovery)})
	LayerTypeEthernetCTP                 = gopacket.RegisterLayerType(12, gopacket.LayerTypeMetadata{"EthernetCTP", gopacket.DecodeFunc(decodeEthernetCTP)})
	LayerTypeEthernetCTPForwardData      = gopacket.RegisterLayerType(13, gopacket.LayerTypeMetadata{"EthernetCTPForwardData", nil})
	LayerTypeEthernetCTPReply            = gopacket.RegisterLayerType(14, gopacket.LayerTypeMetadata{"EthernetCTPReply", nil})
	LayerTypeDot1Q                       = gopacket.RegisterLayerType(15, gopacket.LayerTypeMetadata{"Dot1Q", gopacket.DecodeFunc(decodeDot1Q)})
	LayerTypeEtherIP                     = gopacket.RegisterLayerType(16, gopacket.LayerTypeMetadata{"EtherIP", gopacket.DecodeFunc(decodeEtherIP)})
	LayerTypeEthernet                    = gopacket.RegisterLayerType(17, gopacket.LayerTypeMetadata{"Ethernet", gopacket.DecodeFunc(decodeEthernet)})
	LayerTypeGRE                         = gopacket.RegisterLayerType(18, gopacket.LayerTypeMetadata{"GRE", gopacket.DecodeFunc(decodeGRE)})
	LayerTypeICMPv4                      = gopacket.RegisterLayerType(19, gopacket.LayerTypeMetadata{"ICMPv4", gopacket.DecodeFunc(decodeICMPv4)})
	LayerTypeIPv4                        = gopacket.RegisterLayerType(20, gopacket.LayerTypeMetadata{"IPv4", gopacket.DecodeFunc(decodeIPv4)})
	LayerTypeIPv6                        = gopacket.RegisterLayerType(21, gopacket.LayerTypeMetadata{"IPv6", gopacket.DecodeFunc(decodeIPv6)})
	LayerTypeLLC                         = gopacket.RegisterLayerType(22, gopacket.LayerTypeMetadata{"LLC", gopacket.DecodeFunc(decodeLLC)})
	LayerTypeSNAP                        = gopacket.RegisterLayerType(23, gopacket.LayerTypeMetadata{"SNAP", gopacket.DecodeFunc(decodeSNAP)})
	LayerTypeMPLS                        = gopacket.RegisterLayerType(24, gopacket.LayerTypeMetadata{"MPLS", gopacket.DecodeFunc(decodeMPLS)})
	LayerTypePPP                         = gopacket.RegisterLayerType(25, gopacket.LayerTypeMetadata{"PPP", gopacket.DecodeFunc(decodePPP)})
	LayerTypePPPoE                       = gopacket.RegisterLayerType(26, gopacket.LayerTypeMetadata{"PPPoE", gopacket.DecodeFunc(decodePPPoE)})
	LayerTypeRUDP                        = gopacket.RegisterLayerType(27, gopacket.LayerTypeMetadata{"RUDP", gopacket.DecodeFunc(decodeRUDP)})
	LayerTypeSCTP                        = gopacket.RegisterLayerType(28, gopacket.LayerTypeMetadata{"SCTP", gopacket.DecodeFunc(decodeSCTP)})
	LayerTypeSCTPUnknownChunkType        = gopacket.RegisterLayerType(29, gopacket.LayerTypeMetadata{"SCTPUnknownChunkType", nil})
	LayerTypeSCTPData                    = gopacket.RegisterLayerType(30, gopacket.LayerTypeMetadata{"SCTPData", nil})
	LayerTypeSCTPInit                    = gopacket.RegisterLayerType(31, gopacket.LayerTypeMetadata{"SCTPInit", nil})
	LayerTypeSCTPSack                    = gopacket.RegisterLayerType(32, gopacket.LayerTypeMetadata{"SCTPSack", nil})
	LayerTypeSCTPHeartbeat               = gopacket.RegisterLayerType(33, gopacket.LayerTypeMetadata{"SCTPHeartbeat", nil})
	LayerTypeSCTPError                   = gopacket.RegisterLayerType(34, gopacket.LayerTypeMetadata{"SCTPError", nil})
	LayerTypeSCTPShutdown                = gopacket.RegisterLayerType(35, gopacket.LayerTypeMetadata{"SCTPShutdown", nil})
	LayerTypeSCTPShutdownAck             = gopacket.RegisterLayerType(36, gopacket.LayerTypeMetadata{"SCTPShutdownAck", nil})
	LayerTypeSCTPCookieEcho              = gopacket.RegisterLayerType(37, gopacket.LayerTypeMetadata{"SCTPCookieEcho", nil})
	LayerTypeSCTPEmptyLayer              = gopacket.RegisterLayerType(38, gopacket.LayerTypeMetadata{"SCTPEmptyLayer", nil})
	LayerTypeSCTPInitAck                 = gopacket.RegisterLayerType(39, gopacket.LayerTypeMetadata{"SCTPInitAck", nil})
	LayerTypeSCTPHeartbeatAck            = gopacket.RegisterLayerType(40, gopacket.LayerTypeMetadata{"SCTPHeartbeatAck", nil})
	LayerTypeSCTPAbort                   = gopacket.RegisterLayerType(41, gopacket.LayerTypeMetadata{"SCTPAbort", nil})
	LayerTypeSCTPShutdownComplete        = gopacket.RegisterLayerType(42, gopacket.LayerTypeMetadata{"SCTPShutdownComplete", nil})
	LayerTypeSCTPCookieAck               = gopacket.RegisterLayerType(43, gopacket.LayerTypeMetadata{"SCTPCookieAck", nil})
	LayerTypeTCP                         = gopacket.RegisterLayerType(44, gopacket.LayerTypeMetadata{"TCP", gopacket.DecodeFunc(decodeTCP)})
	LayerTypeUDP                         = gopacket.RegisterLayerType(45, gopacket.LayerTypeMetadata{"UDP", gopacket.DecodeFunc(decodeUDP)})
	LayerTypeIPv6HopByHop                = gopacket.RegisterLayerType(46, gopacket.LayerTypeMetadata{"IPv6HopByHop", gopacket.DecodeFunc(decodeIPv6HopByHop)})
	LayerTypeIPv6Routing                 = gopacket.RegisterLayerType(47, gopacket.LayerTypeMetadata{"IPv6Routing", gopacket.DecodeFunc(decodeIPv6Routing)})
	LayerTypeIPv6Fragment                = gopacket.RegisterLayerType(48, gopacket.LayerTypeMetadata{"IPv6Fragment", gopacket.DecodeFunc(decodeIPv6Fragment)})
	LayerTypeIPv6Destination             = gopacket.RegisterLayerType(49, gopacket.LayerTypeMetadata{"IPv6Destination", gopacket.DecodeFunc(decodeIPv6Destination)})
	LayerTypeIPSecAH                     = gopacket.RegisterLayerType(50, gopacket.LayerTypeMetadata{"IPSecAH", gopacket.DecodeFunc(decodeIPSecAH)})
	LayerTypeIPSecESP                    = gopacket.RegisterLayerType(51, gopacket.LayerTypeMetadata{"IPSecESP", gopacket.DecodeFunc(decodeIPSecESP)})
	LayerTypeUDPLite                     = gopacket.RegisterLayerType(52, gopacket.LayerTypeMetadata{"UDPLite", gopacket.DecodeFunc(decodeUDPLite)})
	LayerTypeFDDI                        = gopacket.RegisterLayerType(53, gopacket.LayerTypeMetadata{"FDDI", gopacket.DecodeFunc(decodeFDDI)})
	LayerTypeLoopback                    = gopacket.RegisterLayerType(54, gopacket.LayerTypeMetadata{"Loopback", gopacket.DecodeFunc(decodeLoopback)})
	LayerTypeEAP                         = gopacket.RegisterLayerType(55, gopacket.LayerTypeMetadata{"EAP", gopacket.DecodeFunc(decodeEAP)})
	LayerTypeEAPOL                       = gopacket.RegisterLayerType(56, gopacket.LayerTypeMetadata{"EAPOL", gopacket.DecodeFunc(decodeEAPOL)})
	LayerTypeICMPv6                      = gopacket.RegisterLayerType(57, gopacket.LayerTypeMetadata{"ICMPv6", gopacket.DecodeFunc(decodeICMPv6)})
	LayerTypeLinkLayerDiscovery          = gopacket.RegisterLayerType(58, gopacket.LayerTypeMetadata{"LinkLayerDiscovery", gopacket.DecodeFunc(decodeLinkLayerDiscovery)})
	LayerTypeCiscoDiscoveryInfo          = gopacket.RegisterLayerType(59, gopacket.LayerTypeMetadata{"CiscoDiscoveryInfo", gopacket.DecodeFunc(decodeCiscoDiscoveryInfo)})
	LayerTypeLinkLayerDiscoveryInfo      = gopacket.RegisterLayerType(60, gopacket.LayerTypeMetadata{"LinkLayerDiscoveryInfo", nil})
	LayerTypeNortelDiscovery             = gopacket.RegisterLayerType(61, gopacket.LayerTypeMetadata{"NortelDiscovery", gopacket.DecodeFunc(decodeNortelDiscovery)})
	LayerTypeIGMP                        = gopacket.RegisterLayerType(62, gopacket.LayerTypeMetadata{"IGMP", gopacket.DecodeFunc(decodeIGMP)})
	LayerTypePFLog                       = gopacket.RegisterLayerType(63, gopacket.LayerTypeMetadata{"PFLog", gopacket.DecodeFunc(decodePFLog)})
	LayerTypeRadioTap                    = gopacket.RegisterLayerType(64, gopacket.LayerTypeMetadata{"RadioTap", gopacket.DecodeFunc(decodeRadioTap)})
	LayerTypeDot11                       = gopacket.RegisterLayerType(65, gopacket.LayerTypeMetadata{"Dot11", gopacket.DecodeFunc(decodeDot11)})
	LayerTypeDot11Ctrl                   = gopacket.RegisterLayerType(66, gopacket.LayerTypeMetadata{"Dot11Ctrl", gopacket.DecodeFunc(decodeDot11Ctrl)})
	LayerTypeDot11Data                   = gopacket.RegisterLayerType(67, gopacket.LayerTypeMetadata{"Dot11Data", gopacket.DecodeFunc(decodeDot11Data)})
	LayerTypeDot11DataCFAck              = gopacket.RegisterLayerType(68, gopacket.LayerTypeMetadata{"Dot11DataCFAck", gopacket.DecodeFunc(decodeDot11DataCFAck)})
	LayerTypeDot11DataCFPoll             = gopacket.RegisterLayerType(69, gopacket.LayerTypeMetadata{"Dot11DataCFPoll", gopacket.DecodeFunc(decodeDot11DataCFPoll)})
	LayerTypeDot11DataCFAckPoll          = gopacket.RegisterLayerType(70, gopacket.LayerTypeMetadata{"Dot11DataCFAckPoll", gopacket.DecodeFunc(decodeDot11DataCFAckPoll)})
	LayerTypeDot11DataNull               = gopacket.RegisterLayerType(71, gopacket.LayerTypeMetadata{"Dot11DataNull", gopacket.DecodeFunc(decodeDot11DataNull)})
	LayerTypeDot11DataCFAckNoData        = gopacket.RegisterLayerType(72, gopacket.LayerTypeMetadata{"Dot11DataCFAck", gopacket.DecodeFunc(decodeDot11DataCFAck)})
	LayerTypeDot11DataCFPollNoData       = gopacket.RegisterLayerType(73, gopacket.LayerTypeMetadata{"Dot11DataCFPoll", gopacket.DecodeFunc(decodeDot11DataCFPoll)})
	LayerTypeDot11DataCFAckPollNoData    = gopacket.RegisterLayerType(74, gopacket.LayerTypeMetadata{"Dot11DataCFAckPoll", gopacket.DecodeFunc(decodeDot11DataCFAckPoll)})
	LayerTypeDot11DataQOSData            = gopacket.RegisterLayerType(75, gopacket.LayerTypeMetadata{"Dot11DataQOSData", gopacket.DecodeFunc(decodeDot11DataQOSData)})
	LayerTypeDot11DataQOSDataCFAck       = gopacket.RegisterLayerType(76, gopacket.LayerTypeMetadata{"Dot11DataQOSDataCFAck", gopacket.DecodeFunc(decodeDot11DataQOSDataCFAck)})
	LayerTypeDot11DataQOSDataCFPoll      = gopacket.RegisterLayerType(77, gopacket.LayerTypeMetadata{"Dot11DataQOSDataCFPoll", gopacket.DecodeFunc(decodeDot11DataQOSDataCFPoll)})
	LayerTypeDot11DataQOSDataCFAckPoll   = gopacket.RegisterLayerType(78, gopacket.LayerTypeMetadata{"Dot11DataQOSDataCFAckPoll", gopacket.DecodeFunc(decodeDot11DataQOSDataCFAckPoll)})
	LayerTypeDot11DataQOSNull            = gopacket.RegisterLayerType(79, gopacket.LayerTypeMetadata{"Dot11DataQOSNull", gopacket.DecodeFunc(decodeDot11DataQOSNull)})
	LayerTypeDot11DataQOSCFPollNoData    = gopacket.RegisterLayerType(80, gopacket.LayerTypeMetadata{"Dot11DataQOSCFPoll", gopacket.DecodeFunc(decodeDot11DataQOSCFPollNoData)})
	LayerTypeDot11DataQOSCFAckPollNoData = gopacket.RegisterLayerType(81, gopacket.LayerTypeMetadata{"Dot11DataQOSCFAckPoll", gopacket.DecodeFunc(decodeDot11DataQOSCFAckPollNoData)})
	LayerTypeDot11InformationElement     = gopacket.RegisterLayerType(82, gopacket.LayerTypeMetadata{"Dot11InformationElement", gopacket.DecodeFunc(decodeDot11InformationElement)})
	LayerTypeDot11CtrlCTS                = gopacket.RegisterLayerType(83, gopacket.LayerTypeMetadata{"Dot11CtrlCTS", gopacket.DecodeFunc(decodeDot11CtrlCTS)})
	LayerTypeDot11CtrlRTS                = gopacket.RegisterLayerType(84, gopacket.LayerTypeMetadata{"Dot11CtrlRTS", gopacket.DecodeFunc(decodeDot11CtrlRTS)})
	LayerTypeDot11CtrlBlockAckReq        = gopacket.RegisterLayerType(85, gopacket.LayerTypeMetadata{"Dot11CtrlBlockAckReq", gopacket.DecodeFunc(decodeDot11CtrlBlockAckReq)})
	LayerTypeDot11CtrlBlockAck           = gopacket.RegisterLayerType(86, gopacket.LayerTypeMetadata{"Dot11CtrlBlockAck", gopacket.DecodeFunc(decodeDot11CtrlBlockAck)})
	LayerTypeDot11CtrlPowersavePoll      = gopacket.RegisterLayerType(87, gopacket.LayerTypeMetadata{"Dot11CtrlPowersavePoll", gopacket.DecodeFunc(decodeDot11CtrlPowersavePoll)})
	LayerTypeDot11CtrlAck                = gopacket.RegisterLayerType(88, gopacket.LayerTypeMetadata{"Dot11CtrlAck", gopacket.DecodeFunc(decodeDot11CtrlAck)})
	LayerTypeDot11CtrlCFEnd              = gopacket.RegisterLayerType(89, gopacket.LayerTypeMetadata{"Dot11CtrlCFEnd", gopacket.DecodeFunc(decodeDot11CtrlCFEnd)})
	LayerTypeDot11CtrlCFEndAck           = gopacket.RegisterLayerType(90, gopacket.LayerTypeMetadata{"Dot11CtrlCFEndAck", gopacket.DecodeFunc(decodeDot11CtrlCFEndAck)})
	LayerTypeDot11MgmtAssociationReq     = gopacket.RegisterLayerType(91, gopacket.LayerTypeMetadata{"Dot11MgmtAssociationReq", gopacket.DecodeFunc(decodeDot11MgmtAssociationReq)})
	LayerTypeDot11MgmtAssociationResp    = gopacket.RegisterLayerType(92, gopacket.LayerTypeMetadata{"Dot11MgmtAssociationResp", gopacket.DecodeFunc(decodeDot11MgmtAssociationResp)})
	LayerTypeDot11MgmtReassociationReq   = gopacket.RegisterLayerType(93, gopacket.LayerTypeMetadata{"Dot11MgmtReassociationReq", gopacket.DecodeFunc(decodeDot11MgmtReassociationReq)})
	LayerTypeDot11MgmtReassociationResp  = gopacket.RegisterLayerType(94, gopacket.LayerTypeMetadata{"Dot11MgmtReassociationResp", gopacket.DecodeFunc(decodeDot11MgmtReassociationResp)})
	LayerTypeDot11MgmtProbeReq           = gopacket.RegisterLayerType(95, gopacket.LayerTypeMetadata{"Dot11MgmtProbeReq", gopacket.DecodeFunc(decodeDot11MgmtProbeReq)})
	LayerTypeDot11MgmtProbeResp          = gopacket.RegisterLayerType(96, gopacket.LayerTypeMetadata{"Dot11MgmtProbeResp", gopacket.DecodeFunc(decodeDot11MgmtProbeResp)})
	LayerTypeDot11MgmtMeasurementPilot   = gopacket.RegisterLayerType(97, gopacket.LayerTypeMetadata{"Dot11MgmtMeasurementPilot", gopacket.DecodeFunc(decodeDot11MgmtMeasurementPilot)})
	LayerTypeDot11MgmtBeacon             = gopacket.RegisterLayerType(98, gopacket.LayerTypeMetadata{"Dot11MgmtBeacon", gopacket.DecodeFunc(decodeDot11MgmtBeacon)})
	LayerTypeDot11MgmtATIM               = gopacket.RegisterLayerType(99, gopacket.LayerTypeMetadata{"Dot11MgmtATIM", gopacket.DecodeFunc(decodeDot11MgmtATIM)})
	LayerTypeDot11MgmtDisassociation     = gopacket.RegisterLayerType(100, gopacket.LayerTypeMetadata{"Dot11MgmtDisassociation", gopacket.DecodeFunc(decodeDot11MgmtDisassociation)})
	LayerTypeDot11MgmtAuthentication     = gopacket.RegisterLayerType(101, gopacket.LayerTypeMetadata{"Dot11MgmtAuthentication", gopacket.DecodeFunc(decodeDot11MgmtAuthentication)})
	LayerTypeDot11MgmtDeauthentication   = gopacket.RegisterLayerType(102, gopacket.LayerTypeMetadata{"Dot11MgmtDeauthentication", gopacket.DecodeFunc(decodeDot11MgmtDeauthentication)})
	LayerTypeDot11MgmtAction             = gopacket.RegisterLayerType(103, gopacket.LayerTypeMetadata{"Dot11MgmtAction", gopacket.DecodeFunc(decodeDot11MgmtAction)})
	LayerTypeDot11MgmtActionNoAck        = gopacket.RegisterLayerType(104, gopacket.LayerTypeMetadata{"Dot11MgmtActionNoAck", gopacket.DecodeFunc(decodeDot11MgmtActionNoAck)})
	LayerTypeDot11MgmtArubaWLAN          = gopacket.RegisterLayerType(105, gopacket.LayerTypeMetadata{"Dot11MgmtArubaWLAN", gopacket.DecodeFunc(decodeDot11MgmtArubaWLAN)})
	LayerTypeDot11WEP                    = gopacket.RegisterLayerType(106, gopacket.LayerTypeMetadata{"Dot11WEP", gopacket.DecodeFunc(decodeDot11WEP)})
	LayerTypeDNS                         = gopacket.RegisterLayerType(107, gopacket.LayerTypeMetadata{"DNS", gopacket.DecodeFunc(decodeDNS)})
	LayerTypeUSB                         = gopacket.RegisterLayerType(108, gopacket.LayerTypeMetadata{"USB", gopacket.DecodeFunc(decodeUSB)})
	LayerTypeUSBRequestBlockSetup        = gopacket.RegisterLayerType(109, gopacket.LayerTypeMetadata{"USBRequestBlockSetup", gopacket.DecodeFunc(decodeUSBRequestBlockSetup)})
	LayerTypeUSBControl                  = gopacket.RegisterLayerType(110, gopacket.LayerTypeMetadata{"USBControl", gopacket.DecodeFunc(decodeUSBControl)})
	LayerTypeUSBInterrupt                = gopacket.RegisterLayerType(111, gopacket.LayerTypeMetadata{"USBInterrupt", gopacket.DecodeFunc(decodeUSBInterrupt)})
	LayerTypeUSBBulk                     = gopacket.RegisterLayerType(112, gopacket.LayerTypeMetadata{"USBBulk", gopacket.DecodeFunc(decodeUSBBulk)})
	LayerTypeLinuxSLL                    = gopacket.RegisterLayerType(113, gopacket.LayerTypeMetadata{"Linux SLL", gopacket.DecodeFunc(decodeLinuxSLL)})
	LayerTypeSFlow                       = gopacket.RegisterLayerType(114, gopacket.LayerTypeMetadata{"SFlow", gopacket.DecodeFunc(decodeSFlow)})
	LayerTypePrismHeader                 = gopacket.RegisterLayerType(115, gopacket.LayerTypeMetadata{"Prism monitor mode header", gopacket.DecodeFunc(decodePrismHeader)})
)

var (
	// LayerClassIPNetwork contains TCP/IP network layer types.
	LayerClassIPNetwork = gopacket.NewLayerClass([]gopacket.LayerType{
		LayerTypeIPv4,
		LayerTypeIPv6,
	})
	// LayerClassIPTransport contains TCP/IP transport layer types.
	LayerClassIPTransport = gopacket.NewLayerClass([]gopacket.LayerType{
		LayerTypeTCP,
		LayerTypeUDP,
		LayerTypeSCTP,
	})
	// LayerClassIPControl contains TCP/IP control protocols.
	LayerClassIPControl = gopacket.NewLayerClass([]gopacket.LayerType{
		LayerTypeICMPv4,
		LayerTypeICMPv6,
	})
	// LayerClassSCTPChunk contains SCTP chunk types (not the top-level SCTP
	// layer).
	LayerClassSCTPChunk = gopacket.NewLayerClass([]gopacket.LayerType{
		LayerTypeSCTPUnknownChunkType,
		LayerTypeSCTPData,
		LayerTypeSCTPInit,
		LayerTypeSCTPSack,
		LayerTypeSCTPHeartbeat,
		LayerTypeSCTPError,
		LayerTypeSCTPShutdown,
		LayerTypeSCTPShutdownAck,
		LayerTypeSCTPCookieEcho,
		LayerTypeSCTPEmptyLayer,
		LayerTypeSCTPInitAck,
		LayerTypeSCTPHeartbeatAck,
		LayerTypeSCTPAbort,
		LayerTypeSCTPShutdownComplete,
		LayerTypeSCTPCookieAck,
	})
	// LayerClassIPv6Extension contains IPv6 extension headers.
	LayerClassIPv6Extension = gopacket.NewLayerClass([]gopacket.LayerType{
		LayerTypeIPv6HopByHop,
		LayerTypeIPv6Routing,
		LayerTypeIPv6Fragment,
		LayerTypeIPv6Destination,
	})
	LayerClassIPSec = gopacket.NewLayerClass([]gopacket.LayerType{
		LayerTypeIPSecAH,
		LayerTypeIPSecESP,
	})
)
