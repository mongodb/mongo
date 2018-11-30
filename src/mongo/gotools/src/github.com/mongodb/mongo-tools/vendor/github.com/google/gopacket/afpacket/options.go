// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// +build linux

package afpacket

import (
	"fmt"
	"time"
)

// #include <linux/if_packet.h>
// #include <sys/socket.h>
import "C"

// OptTPacketVersion is the version of TPacket to use.
// It can be passed into NewTPacket.
type OptTPacketVersion int

// String returns a string representation of the version, generally of the form V#.
func (t OptTPacketVersion) String() string {
	switch t {
	case TPacketVersion1:
		return "V1"
	case TPacketVersion2:
		return "V2"
	case TPacketVersion3:
		return "V3"
	case TPacketVersionHighestAvailable:
		return "HighestAvailable"
	}
	return "InvalidVersion"
}

// OptSockType is the socket type used to open the TPacket socket.
type OptSocketType int

func (t OptSocketType) String() string {
	switch t {
	case SocketRaw:
		return "SOCK_RAW"
	case SocketDgram:
		return "SOCK_DGRAM"
	}
	return "UnknownSocketType"
}

const (
	// TPacketVersionHighestAvailable tells NewHandle to use the highest available version of tpacket the kernel has available.
	// This is the default, should a version number not be given in NewHandle's options.
	TPacketVersionHighestAvailable = OptTPacketVersion(-1)
	TPacketVersion1                = OptTPacketVersion(C.TPACKET_V1)
	TPacketVersion2                = OptTPacketVersion(C.TPACKET_V2)
	TPacketVersion3                = OptTPacketVersion(C.TPACKET_V3)
	tpacketVersionMax              = TPacketVersion3
	tpacketVersionMin              = -1
	// SocketRaw is the default socket type.  It returns packet data
	// including the link layer (ethernet headers, etc).
	SocketRaw = OptSocketType(C.SOCK_RAW)
	// SocketDgram strips off the link layer when reading packets, and adds
	// the link layer back automatically on packet writes (coming soon...)
	SocketDgram = OptSocketType(C.SOCK_DGRAM)
)

// OptInterface is the specific interface to bind to.
// It can be passed into NewTPacket.
type OptInterface string

// OptFrameSize is TPacket's tp_frame_size
// It can be passed into NewTPacket.
type OptFrameSize int

// OptBlockSize is TPacket's tp_block_size
// It can be passed into NewTPacket.
type OptBlockSize int

// OptNumBlocks is TPacket's tp_block_nr
// It can be passed into NewTPacket.
type OptNumBlocks int

// OptBlockTimeout is TPacket v3's tp_retire_blk_tov.  Note that it has only millisecond granularity, so must be >= 1 ms.
// It can be passed into NewTPacket.
type OptBlockTimeout time.Duration

const (
	DefaultFrameSize    = 4096                   // Default value for OptFrameSize.
	DefaultBlockSize    = DefaultFrameSize * 128 // Default value for OptBlockSize.
	DefaultNumBlocks    = 128                    // Default value for OptNumBlocks.
	DefaultBlockTimeout = 64 * time.Millisecond  // Default value for OptBlockTimeout.
)

type options struct {
	frameSize      int
	framesPerBlock int
	blockSize      int
	numBlocks      int
	blockTimeout   time.Duration
	version        OptTPacketVersion
	socktype       OptSocketType
	iface          string
}

var defaultOpts = options{
	frameSize:    DefaultFrameSize,
	blockSize:    DefaultBlockSize,
	numBlocks:    DefaultNumBlocks,
	blockTimeout: DefaultBlockTimeout,
	version:      TPacketVersionHighestAvailable,
	socktype:     SocketRaw,
}

func parseOptions(opts ...interface{}) (ret options, err error) {
	ret = defaultOpts
	for _, opt := range opts {
		switch v := opt.(type) {
		case OptFrameSize:
			ret.frameSize = int(v)
		case OptBlockSize:
			ret.blockSize = int(v)
		case OptNumBlocks:
			ret.numBlocks = int(v)
		case OptBlockTimeout:
			ret.blockTimeout = time.Duration(v)
		case OptTPacketVersion:
			ret.version = v
		case OptInterface:
			ret.iface = string(v)
		case OptSocketType:
			ret.socktype = v
		default:
			err = fmt.Errorf("unknown type in options")
			return
		}
	}
	if err = ret.check(); err != nil {
		return
	}
	ret.framesPerBlock = ret.blockSize / ret.frameSize
	return
}
func (o options) check() error {
	switch {
	case o.blockSize%pageSize != 0:
		return fmt.Errorf("block size %d must be divisible by page size %d", o.blockSize, pageSize)
	case o.blockSize%o.frameSize != 0:
		return fmt.Errorf("block size %d must be divisible by frame size %d", o.blockSize, o.frameSize)
	case o.numBlocks < 1:
		return fmt.Errorf("num blocks %d must be >= 1", o.numBlocks)
	case o.blockTimeout < time.Millisecond:
		return fmt.Errorf("block timeout %v must be > %v", o.blockTimeout, time.Millisecond)
	case o.version < tpacketVersionMin || o.version > tpacketVersionMax:
		return fmt.Errorf("tpacket version %v is invalid", o.version)
	}
	return nil
}
