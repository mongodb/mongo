// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"fmt"
	"strconv"

	"github.com/google/gopacket"
)

// TCPPort is a port in a TCP layer.
type TCPPort uint16

// UDPPort is a port in a UDP layer.
type UDPPort uint16

// RUDPPort is a port in a RUDP layer.
type RUDPPort uint8

// SCTPPort is a port in a SCTP layer.
type SCTPPort uint16

// UDPLitePort is a port in a UDPLite layer.
type UDPLitePort uint16

// RUDPPortNames contains the string names for all RUDP ports.
var RUDPPortNames = map[RUDPPort]string{}

// UDPLitePortNames contains the string names for all UDPLite ports.
var UDPLitePortNames = map[UDPLitePort]string{}

// {TCP,UDP,SCTP}PortNames can be found in iana_ports.go

// String returns the port as "number(name)" if there's a well-known port name,
// or just "number" if there isn't.  Well-known names are stored in
// TCPPortNames.
func (a TCPPort) String() string {
	if name, ok := TCPPortNames[a]; ok {
		return fmt.Sprintf("%d(%s)", a, name)
	}
	return strconv.Itoa(int(a))
}

// String returns the port as "number(name)" if there's a well-known port name,
// or just "number" if there isn't.  Well-known names are stored in
// UDPPortNames.
func (a UDPPort) String() string {
	if name, ok := UDPPortNames[a]; ok {
		return fmt.Sprintf("%d(%s)", a, name)
	}
	return strconv.Itoa(int(a))
}

// LayerType returns a LayerType that would be able to decode the
// application payload. It use some well-known port such as 53 for DNS.
//
// Returns gopacket.LayerTypePayload for unknown/unsupported port numbers.
func (a UDPPort) LayerType() gopacket.LayerType {
	switch a {
	case 53:
		return LayerTypeDNS
	case 6343:
		return LayerTypeSFlow
	default:
		return gopacket.LayerTypePayload
	}
}

// String returns the port as "number(name)" if there's a well-known port name,
// or just "number" if there isn't.  Well-known names are stored in
// RUDPPortNames.
func (a RUDPPort) String() string {
	if name, ok := RUDPPortNames[a]; ok {
		return fmt.Sprintf("%d(%s)", a, name)
	}
	return strconv.Itoa(int(a))
}

// String returns the port as "number(name)" if there's a well-known port name,
// or just "number" if there isn't.  Well-known names are stored in
// SCTPPortNames.
func (a SCTPPort) String() string {
	if name, ok := SCTPPortNames[a]; ok {
		return fmt.Sprintf("%d(%s)", a, name)
	}
	return strconv.Itoa(int(a))
}

// String returns the port as "number(name)" if there's a well-known port name,
// or just "number" if there isn't.  Well-known names are stored in
// UDPLitePortNames.
func (a UDPLitePort) String() string {
	if name, ok := UDPLitePortNames[a]; ok {
		return fmt.Sprintf("%d(%s)", a, name)
	}
	return strconv.Itoa(int(a))
}
