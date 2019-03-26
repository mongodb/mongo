// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.
// +build linux

package pcapgo

import (
	"net"
	"syscall"
	"time"

	"github.com/google/gopacket"
	"github.com/mdlayher/raw"
)

// EthernetHandle wraps a raw.Conn, implementing gopacket.PacketDataSource so
// that the handle can be used with gopacket.NewPacketSource.
type EthernetHandle struct {
	*raw.Conn
}

// ReadPacketData implements gopacket.PacketDataSource.
func (h *EthernetHandle) ReadPacketData() ([]byte, gopacket.CaptureInfo, error) {
	b := make([]byte, 4096) // TODO(correctness): how much space do we need?
	n, _, err := h.ReadFrom(b)
	if err != nil {
		return nil, gopacket.CaptureInfo{}, err
	}
	data := b[:n]
	return data, gopacket.CaptureInfo{
		CaptureLength: len(data),
		Length:        len(data),
		Timestamp:     time.Now(),
	}, nil
}

// NewEthernetHandle implements pcap.OpenLive for ethernet interfaces only.
func NewEthernetHandle(ifname string) (*EthernetHandle, error) {
	intf, err := net.InterfaceByName(ifname)
	if err != nil {
		return nil, err
	}

	conn, err := raw.ListenPacket(intf, syscall.ETH_P_ALL, nil)
	if err != nil {
		return nil, err
	}

	return &EthernetHandle{conn}, nil
}
