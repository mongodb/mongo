// Copyright 2014 Damjan Cvetko. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package pcapgo

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
)

// Reader wraps an underlying io.Reader to read packet data in PCAP
// format.  See http://wiki.wireshark.org/Development/LibpcapFileFormat
// for information on the file format.
//
// We currenty read v2.4 file format with nanosecond and microsecdond
// timestamp resolution in little-endian and big-endian encoding.
type Reader struct {
	r              io.Reader
	byteOrder      binary.ByteOrder
	nanoSecsFactor uint32
	versionMajor   uint16
	versionMinor   uint16
	// timezone
	// sigfigs
	snaplen  uint32
	linkType layers.LinkType
	// reusable buffer
	buf []byte
}

const magicNanoseconds = 0xA1B23C4D
const magicMicrosecondsBigendian = 0xD4C3B2A1
const magicNanosecondsBigendian = 0x4D3CB2A1

// NewReader returns a new reader object, for reading packet data from
// the given reader. The reader must be open and header data is
// read from it at this point.
// If the file format is not supported an error is returned
//
//  // Create new reader:
//  f, _ := os.Open("/tmp/file.pcap")
//  defer f.Close()
//  r, err := NewReader(f)
//  data, ci, err := r.ReadPacketData()
func NewReader(r io.Reader) (*Reader, error) {
	ret := Reader{r: r}
	if err := ret.readHeader(); err != nil {
		return nil, err
	}
	return &ret, nil
}

func (r *Reader) readHeader() error {
	buf := make([]byte, 24)
	if n, err := io.ReadFull(r.r, buf); err != nil {
		return err
	} else if n < 24 {
		return errors.New("Not enough data for read")
	}
	if magic := binary.LittleEndian.Uint32(buf[0:4]); magic == magicNanoseconds {
		r.byteOrder = binary.LittleEndian
		r.nanoSecsFactor = 1
	} else if magic == magicNanosecondsBigendian {
		r.byteOrder = binary.BigEndian
		r.nanoSecsFactor = 1
	} else if magic == magicMicroseconds {
		r.byteOrder = binary.LittleEndian
		r.nanoSecsFactor = 1000
	} else if magic == magicMicrosecondsBigendian {
		r.byteOrder = binary.BigEndian
		r.nanoSecsFactor = 1000
	} else {
		return errors.New(fmt.Sprintf("Unknown maigc %x", magic))
	}
	if r.versionMajor = r.byteOrder.Uint16(buf[4:6]); r.versionMajor != versionMajor {
		return errors.New(fmt.Sprintf("Unknown major version %d", r.versionMajor))
	}
	if r.versionMinor = r.byteOrder.Uint16(buf[6:8]); r.versionMinor != versionMinor {
		return errors.New(fmt.Sprintf("Unknown minor version %d", r.versionMinor))
	}
	// ignore timezone 8:12 and sigfigs 12:16
	r.snaplen = r.byteOrder.Uint32(buf[16:20])
	r.buf = make([]byte, r.snaplen+16)
	r.linkType = layers.LinkType(r.byteOrder.Uint32(buf[20:24]))
	return nil
}

// Read next packet from file
func (r *Reader) ReadPacketData() (data []byte, ci gopacket.CaptureInfo, err error) {
	if ci, err = r.readPacketHeader(); err != nil {
		return
	}

	var n int
	data = r.buf[16 : 16+ci.CaptureLength]
	if n, err = io.ReadFull(r.r, data); err != nil {
		return
	} else if n < ci.CaptureLength {
		err = io.ErrUnexpectedEOF
	}
	return
}

func (r *Reader) readPacketHeader() (ci gopacket.CaptureInfo, err error) {
	var n int
	if n, err = io.ReadFull(r.r, r.buf[0:16]); err != nil {
		return
	} else if n < 16 {
		err = io.ErrUnexpectedEOF
		return
	}
	ci.Timestamp = time.Unix(int64(r.byteOrder.Uint32(r.buf[0:4])), int64(r.byteOrder.Uint32(r.buf[4:8])*r.nanoSecsFactor)).UTC()
	ci.CaptureLength = int(r.byteOrder.Uint32(r.buf[8:12]))
	ci.Length = int(r.byteOrder.Uint32(r.buf[12:16]))
	return
}

// LinkType returns network, as a layers.LinkType.
func (r *Reader) LinkType() layers.LinkType {
	return r.linkType
}

// Reader formater
func (r *Reader) String() string {
	return fmt.Sprintf("PcapFile  maj: %x min: %x snaplen: %d linktype: %s", r.versionMajor, r.versionMinor, r.snaplen, r.linkType)
}
