// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package pcapgo

import (
	"bytes"
	"github.com/google/gopacket"
	"testing"
	"time"
)

func TestWriteHeader(t *testing.T) {
	var buf bytes.Buffer
	w := NewWriter(&buf)
	w.WriteFileHeader(0x1234, 0x56)
	want := []byte{
		0xd4, 0xc3, 0xb2, 0xa1, 0x02, 0x00, 0x04, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x34, 0x12, 0x00, 0x00, 0x56, 0x00, 0x00, 0x00,
	}
	if got := buf.Bytes(); !bytes.Equal(got, want) {
		t.Errorf("buf mismatch:\nwant: %+v\ngot:  %+v", want, got)
	}
}

func TestWritePacket(t *testing.T) {
	ci := gopacket.CaptureInfo{
		Timestamp:     time.Unix(0x01020304, 0xAA*1000),
		Length:        0xABCD,
		CaptureLength: 10,
	}
	data := []byte{9, 8, 7, 6, 5, 4, 3, 2, 1, 0}
	var buf bytes.Buffer
	w := NewWriter(&buf)
	w.WritePacket(ci, data)
	want := []byte{
		0x04, 0x03, 0x02, 0x01, 0xAA, 0x00, 0x00, 0x00,
		0x0A, 0x00, 0x00, 0x00, 0xCD, 0xAB, 0x00, 0x00,
		0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
	}
	if got := buf.Bytes(); !bytes.Equal(got, want) {
		t.Errorf("buf mismatch:\nwant: %+v\ngot:  %+v", want, got)
	}
}

func TestCaptureInfoErrors(t *testing.T) {
	data := []byte{1, 2, 3, 4}
	ts := time.Unix(0, 0)
	for _, test := range []gopacket.CaptureInfo{
		gopacket.CaptureInfo{
			Timestamp:     ts,
			Length:        5,
			CaptureLength: 5,
		},
		gopacket.CaptureInfo{
			Timestamp:     ts,
			Length:        3,
			CaptureLength: 4,
		},
	} {
		var buf bytes.Buffer
		w := NewWriter(&buf)
		if err := w.WritePacket(test, data); err == nil {
			t.Errorf("CaptureInfo %+v should have error", test)
		}
	}
}
