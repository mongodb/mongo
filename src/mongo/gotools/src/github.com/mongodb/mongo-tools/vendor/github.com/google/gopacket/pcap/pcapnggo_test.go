// Copyright 2018 The GoPacket Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package pcap

import (
	"bytes"
	"io/ioutil"
	"reflect"
	"testing"
	"time"

	"github.com/google/gopacket"
	"github.com/google/gopacket/layers"
	"github.com/google/gopacket/pcapgo"
)

func TestPCAPGoNgWrite(t *testing.T) {
	f, err := ioutil.TempFile("", "pcapnggo")
	if err != nil {
		t.Fatal(err)
	}
	data := []byte{0xab, 0xcd, 0xef, 0x01, 0x02, 0x03, 0x04}
	ci := gopacket.CaptureInfo{
		Timestamp:     time.Unix(12345667, 1234567123),
		Length:        700,
		CaptureLength: len(data),
	}
	func() {
		defer f.Close()
		w, err := pcapgo.NewNgWriter(f, layers.LinkTypeEthernet)
		if err != nil {
			t.Fatal(err)
		}
		if err := w.WritePacket(ci, data); err != nil {
			t.Fatal(err)
		}
		if err := w.Flush(); err != nil {
			t.Fatal(err)
		}
	}()
	h, err := OpenOffline(f.Name())
	if err != nil {
		t.Fatal(err)
	}
	defer h.Close()
	gotData, gotCI, err := h.ReadPacketData()
	if err != nil {
		t.Fatal("could not read first packet:", err)
	}
	if !bytes.Equal(gotData, data) {
		t.Errorf("byte mismatch:\nwant: %v\n got: %v", data, gotData)
	}
	if !reflect.DeepEqual(ci, gotCI) {
		t.Errorf("CI mismatch:\nwant: %v\n got: %v", ci, gotCI)
	}
}
