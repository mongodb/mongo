// Copyright 2016, Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package layers

import (
	"testing"

	"github.com/google/gopacket"
)

func TestTCPOptionKindString(t *testing.T) {
	testData := []struct {
		o *TCPOption
		s string
	}{
		{&TCPOption{
			OptionType:   TCPOptionKindNop,
			OptionLength: 1,
		},
			"TCPOption(NOP:)"},
		{&TCPOption{
			OptionType:   TCPOptionKindMSS,
			OptionLength: 4,
			OptionData:   []byte{0x12, 0x34},
		},
			"TCPOption(MSS:4660 0x1234)"},
		{&TCPOption{
			OptionType:   TCPOptionKindTimestamps,
			OptionLength: 10,
			OptionData:   []byte{0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01},
		},
			"TCPOption(Timestamps:2/1 0x0000000200000001)"}}

	for _, tc := range testData {
		if s := tc.o.String(); s != tc.s {
			t.Errorf("expected %#v string to be %s, got %s", tc.o, tc.s, s)
		}
	}
}

func TestTCPSerializePadding(t *testing.T) {
	tcp := &TCP{}
	tcp.Options = append(tcp.Options, TCPOption{
		OptionType:   TCPOptionKindNop,
		OptionLength: 1,
	})
	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{FixLengths: true}
	err := gopacket.SerializeLayers(buf, opts, tcp)
	if err != nil {
		t.Fatal(err)
	}
	if len(buf.Bytes())%4 != 0 {
		t.Errorf("TCP data of len %d not padding to 32 bit boundary", len(buf.Bytes()))
	}
}
