// Copyright 2016, Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"bytes"
	"net"
	"testing"

	"github.com/google/gopacket"
)

func TestDHCPv4EncodeRequest(t *testing.T) {
	dhcp := &DHCPv4{Operation: DHCPOpRequest, HardwareType: LinkTypeEthernet, Xid: 0x12345678,
		ClientIP: net.IP{0, 0, 0, 0}, YourClientIP: net.IP{0, 0, 0, 0}, NextServerIP: net.IP{0, 0, 0, 0}, RelayAgentIP: net.IP{0, 0, 0, 0},
		ClientHWAddr: net.HardwareAddr{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc},
		ServerName:   make([]byte, 64), File: make([]byte, 128)}

	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptMessageType, []byte{byte(DHCPMsgTypeDiscover)}))
	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptHostname, []byte{'e', 'x', 'a', 'm', 'p', 'l', 'e', '.', 'c', 'o', 'm'}))
	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptPad, nil))
	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptParamsRequest,
		[]byte{byte(DHCPOptSubnetMask), byte(DHCPOptBroadcastAddr), byte(DHCPOptTimeOffset),
			byte(DHCPOptRouter), byte(DHCPOptDomainName), byte(DHCPOptDNS), byte(DHCPOptDomainSearch),
			byte(DHCPOptHostname), byte(DHCPOptNetBIOSTCPNS), byte(DHCPOptInterfaceMTU), byte(DHCPOptClasslessStaticRoute),
			byte(DHCPOptNTPServers)}))

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true}
	err := gopacket.SerializeLayers(buf, opts, dhcp)
	if err != nil {
		t.Fatal(err)
	}

	p2 := gopacket.NewPacket(buf.Bytes(), LayerTypeDHCPv4, testDecodeOptions)
	dhcp2 := p2.Layer(LayerTypeDHCPv4).(*DHCPv4)
	testDHCPEqual(t, dhcp, dhcp2)
}

func TestDHCPv4EncodeResponse(t *testing.T) {
	dhcp := &DHCPv4{Operation: DHCPOpReply, HardwareType: LinkTypeEthernet, Xid: 0x12345678,
		ClientIP: net.IP{0, 0, 0, 0}, YourClientIP: net.IP{192, 168, 0, 123}, NextServerIP: net.IP{192, 168, 0, 1}, RelayAgentIP: net.IP{0, 0, 0, 0},
		ClientHWAddr: net.HardwareAddr{0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc},
		ServerName:   make([]byte, 64), File: make([]byte, 128)}

	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptMessageType, []byte{byte(DHCPMsgTypeOffer)}))
	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptSubnetMask, []byte{255, 255, 255, 0}))
	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptPad, nil))
	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptT1, []byte{0x00, 0x00, 0x0e, 0x10}))
	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptT2, []byte{0x00, 0x00, 0x0e, 0x10}))
	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptLeaseTime, []byte{0x00, 0x00, 0x0e, 0x10}))
	dhcp.Options = append(dhcp.Options, NewDHCPOption(DHCPOptServerID, []byte{192, 168, 0, 1}))

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true}
	err := gopacket.SerializeLayers(buf, opts, dhcp)
	if err != nil {
		t.Fatal(err)
	}

	p2 := gopacket.NewPacket(buf.Bytes(), LayerTypeDHCPv4, testDecodeOptions)
	dhcp2 := p2.Layer(LayerTypeDHCPv4).(*DHCPv4)
	testDHCPEqual(t, dhcp, dhcp2)
}

func TestDHCPv4DecodeOption(t *testing.T) {
	var tests = []struct {
		msg string
		buf []byte
		err error
	}{
		{
			msg: "DHCPOptPad",
			buf: []byte{0},
			err: nil,
		},
		{
			msg: "Option with zero length",
			buf: []byte{119, 0},
			err: nil,
		},
		{
			msg: "Option with maximum length",
			buf: bytes.Join([][]byte{
				{119, 255},
				bytes.Repeat([]byte{0}, 255),
			}, nil),
			err: nil,
		},
		{
			msg: "Too short option",
			buf: []byte{},
			err: DecOptionNotEnoughData,
		},
		{
			msg: "Too short option when option is not 0 or 255",
			buf: []byte{119},
			err: DecOptionNotEnoughData,
		},
		{
			msg: "Malformed option",
			buf: []byte{119, 1},
			err: DecOptionMalformed,
		},
	}

	for i := range tests {
		var (
			opt = new(DHCPOption)
			err = opt.decode(tests[i].buf)
		)
		if want, got := tests[i].err, err; want != got {
			t.Errorf("[#%v %v] Unexpected error want: %v, got: %v\n", i, tests[i].msg, want, err)
		}
	}
}

func testDHCPEqual(t *testing.T, d1, d2 *DHCPv4) {
	if d1.Operation != d2.Operation {
		t.Errorf("expected Operation=%s, got %s", d1.Operation, d2.Operation)
	}
	if d1.HardwareType != d2.HardwareType {
		t.Errorf("expected HardwareType=%s, got %s", d1.HardwareType, d2.HardwareType)
	}
	if d1.HardwareLen != d2.HardwareLen {
		t.Errorf("expected HardwareLen=%v, got %v", d1.HardwareLen, d2.HardwareLen)
	}
	if d1.HardwareOpts != d2.HardwareOpts {
		t.Errorf("expected HardwareOpts=%v, got %v", d1.HardwareOpts, d2.HardwareOpts)
	}
	if d1.Xid != d2.Xid {
		t.Errorf("expected Xid=%v, got %v", d1.Xid, d2.Xid)
	}
	if d1.Secs != d2.Secs {
		t.Errorf("expected Secs=%v, got %v", d1.Secs, d2.Secs)
	}
	if d1.Flags != d2.Flags {
		t.Errorf("expected Flags=%v, got %v", d1.Flags, d2.Flags)
	}
	if !d1.ClientIP.Equal(d2.ClientIP) {
		t.Errorf("expected ClientIP=%v, got %v", d1.ClientIP, d2.ClientIP)
	}
	if !d1.YourClientIP.Equal(d2.YourClientIP) {
		t.Errorf("expected YourClientIP=%v, got %v", d1.YourClientIP, d2.YourClientIP)
	}
	if !d1.NextServerIP.Equal(d2.NextServerIP) {
		t.Errorf("expected NextServerIP=%v, got %v", d1.NextServerIP, d2.NextServerIP)
	}
	if !d1.RelayAgentIP.Equal(d2.RelayAgentIP) {
		t.Errorf("expected RelayAgentIP=%v, got %v", d1.RelayAgentIP, d2.RelayAgentIP)
	}
	if !bytes.Equal(d1.ClientHWAddr, d2.ClientHWAddr) {
		t.Errorf("expected ClientHWAddr=%v, got %v", d1.ClientHWAddr, d2.ClientHWAddr)
	}
	if !bytes.Equal(d1.ServerName, d2.ServerName) {
		t.Errorf("expected ServerName=%v, got %v", d1.ServerName, d2.ServerName)
	}
	if !bytes.Equal(d1.File, d2.File) {
		t.Errorf("expected File=%v, got %v", d1.File, d2.File)
	}
	if len(d1.Options) != len(d2.Options) {
		t.Errorf("expected %d options, got %d", len(d1.Options), len(d2.Options))
	}

	for i, o := range d1.Options {
		testDHCPOptionEqual(t, i, o, d2.Options[i])
	}
}

func testDHCPOptionEqual(t *testing.T, idx int, d1, d2 DHCPOption) {
	if d1.Type != d2.Type {
		t.Errorf("expection Options[%d].Type = %s, got %s", idx, d1.Type, d2.Type)
	}
	if !bytes.Equal(d1.Data, d2.Data) {
		t.Errorf("expection Options[%d].Data to be = %v, got %v", idx, d1.Data, d2.Data)
	}
}
