// Copyright 2018, Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"bytes"
	"testing"

	"github.com/google/gopacket"
)

func TestDHCPv6EncodeRequest(t *testing.T) {
	dhcpv6 := &DHCPv6{MsgType: DHCPv6MsgTypeRequest, HopCount: 0, TransactionID: []byte{87, 25, 88}}

	client := &DHCPv6DUID{Type: DHCPv6DUIDTypeLLT, HardwareType: []byte{0, 1}, Time: []byte{28, 56, 38, 45}, LinkLayerAddress: []byte{8, 0, 39, 254, 143, 149}}
	dhcpv6.Options = append(dhcpv6.Options, NewDHCPv6Option(DHCPv6OptClientID, client.Encode()))

	server := &DHCPv6DUID{Type: DHCPv6DUIDTypeLLT, HardwareType: []byte{0, 1}, Time: []byte{28, 56, 37, 232}, LinkLayerAddress: []byte{8, 0, 39, 212, 16, 187}}
	dhcpv6.Options = append(dhcpv6.Options, NewDHCPv6Option(DHCPv6OptServerID, server.Encode()))

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true}
	err := gopacket.SerializeLayers(buf, opts, dhcpv6)
	if err != nil {
		t.Fatal(err)
	}

	p2 := gopacket.NewPacket(buf.Bytes(), LayerTypeDHCPv6, testDecodeOptions)
	dhcpv62 := p2.Layer(LayerTypeDHCPv6).(*DHCPv6)
	testDHCPv6Equal(t, dhcpv6, dhcpv62)
}

func TestDHCPv6EncodeReply(t *testing.T) {
	dhcpv6 := &DHCPv6{MsgType: DHCPv6MsgTypeReply, HopCount: 0, TransactionID: []byte{87, 25, 88}}

	client := &DHCPv6DUID{Type: DHCPv6DUIDTypeLLT, HardwareType: []byte{0, 1}, Time: []byte{28, 56, 38, 45}, LinkLayerAddress: []byte{8, 0, 39, 254, 143, 149}}
	dhcpv6.Options = append(dhcpv6.Options, NewDHCPv6Option(DHCPv6OptClientID, client.Encode()))

	server := &DHCPv6DUID{Type: DHCPv6DUIDTypeLLT, HardwareType: []byte{0, 1}, Time: []byte{28, 56, 37, 232}, LinkLayerAddress: []byte{8, 0, 39, 212, 16, 187}}
	dhcpv6.Options = append(dhcpv6.Options, NewDHCPv6Option(DHCPv6OptServerID, server.Encode()))

	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true}
	err := gopacket.SerializeLayers(buf, opts, dhcpv6)
	if err != nil {
		t.Fatal(err)
	}

	p2 := gopacket.NewPacket(buf.Bytes(), LayerTypeDHCPv6, testDecodeOptions)
	dhcpv62 := p2.Layer(LayerTypeDHCPv6).(*DHCPv6)
	testDHCPv6Equal(t, dhcpv6, dhcpv62)
}

func testDHCPv6Equal(t *testing.T, d1, d2 *DHCPv6) {
	if d1.MsgType != d2.MsgType {
		t.Errorf("expected MsgType=%s, got %s", d1.MsgType, d2.MsgType)
	}
	if d1.HopCount != d2.HopCount {
		t.Errorf("expected HopCount=%d, got %d", d1.HopCount, d2.HopCount)
	}
	if !d1.LinkAddr.Equal(d2.LinkAddr) {
		t.Errorf("expected LinkAddr=%v, got %v", d1.LinkAddr, d2.LinkAddr)
	}
	if !d1.PeerAddr.Equal(d2.PeerAddr) {
		t.Errorf("expected PeerAddr=%v, got %v", d1.PeerAddr, d2.PeerAddr)
	}
	if !bytes.Equal(d1.TransactionID, d2.TransactionID) {
		t.Errorf("expected TransactionID=%v, got %v", d1.TransactionID, d2.TransactionID)
	}
	if len(d1.Options) != len(d2.Options) {
		t.Errorf("expected %d options, got %d", len(d1.Options), len(d2.Options))
	}

	for i, o := range d1.Options {
		testDHCPv6OptionEqual(t, i, o, d2.Options[i])
	}
}

func testDHCPv6OptionEqual(t *testing.T, idx int, d1, d2 DHCPv6Option) {
	if d1.Code != d2.Code {
		t.Errorf("expection Options[%d].Code = %s, got %s", idx, d1.Code, d2.Code)
	}
	if d1.Length != d2.Length {
		t.Errorf("expection Options[%d].Length = %d, got %d", idx, d1.Length, d2.Length)
	}
	if !bytes.Equal(d1.Data, d2.Data) {
		t.Errorf("expection Options[%d].Data to be = %v, got %v", idx, d1.Data, d2.Data)
	}
}
