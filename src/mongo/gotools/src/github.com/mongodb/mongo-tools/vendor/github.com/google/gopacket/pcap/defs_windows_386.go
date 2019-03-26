// Copyright 2019 The GoPacket Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// This file contains necessary structs/constants generated from libpcap headers with cgo -godefs
// generated with: generate_defs.exe
// DO NOT MODIFY

package pcap

import "syscall"

const errorBufferSize = 0x100

const (
	pcapErrorNotActivated    = -0x3
	pcapErrorActivated       = -0x4
	pcapWarningPromisc       = 0x2
	pcapErrorNoSuchDevice    = -0x5
	pcapErrorDenied          = -0x8
	pcapErrorNotUp           = -0x9
	pcapError                = -0x1
	pcapWarning              = 0x1
	pcapDIN                  = 0x1
	pcapDOUT                 = 0x2
	pcapDINOUT               = 0x0
	pcapNetmaskUnknown       = 0xffffffff
	pcapTstampPrecisionMicro = 0x0
	pcapTstampPrecisionNano  = 0x1
)

type timeval struct {
	Sec  int32
	Usec int32
}
type pcapPkthdr struct {
	Ts     timeval
	Caplen uint32
	Len    uint32
}
type pcapTPtr uintptr
type pcapBpfInstruction struct {
	Code uint16
	Jt   uint8
	Jf   uint8
	K    uint32
}
type pcapBpfProgram struct {
	Len   uint32
	Insns *pcapBpfInstruction
}
type pcapStats struct {
	Recv   uint32
	Drop   uint32
	Ifdrop uint32
}
type pcapCint int32
type pcapIf struct {
	Next        *pcapIf
	Name        *int8
	Description *int8
	Addresses   *pcapAddr
	Flags       uint32
}

type pcapAddr struct {
	Next      *pcapAddr
	Addr      *syscall.RawSockaddr
	Netmask   *syscall.RawSockaddr
	Broadaddr *syscall.RawSockaddr
	Dstaddr   *syscall.RawSockaddr
}
