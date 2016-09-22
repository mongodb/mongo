// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// This file tests some of the functionality provided in the ip4.go

package layers

import (
	"testing"
)

// Test the function getIPv4OptionSize when the ipv4 has no options
func TestGetIPOptLengthNoOpt(t *testing.T) {
	ip := IPv4{}
	length := ip.getIPv4OptionSize()
	if length != 0 {
		t.Fatalf("Empty option list should have 0 length.  Actual %d", length)
	}
}

// Test the function getIPv4OptionSize when the ipv4 has end of list option
func TestGetIPOptLengthEndOfList(t *testing.T) {
	ip := IPv4{}
	ip.Options = append(ip.Options, IPv4Option{OptionType: 0, OptionLength: 1})
	length := ip.getIPv4OptionSize()
	if length != 4 {
		t.Fatalf("After padding, the list should have 4 length.  Actual %d", length)
	}
}

// Test the function getIPv4OptionSize when the ipv4 has padding and end of list option
func TestGetIPOptLengthPaddingEndOfList(t *testing.T) {
	ip := IPv4{}
	ip.Options = append(ip.Options, IPv4Option{OptionType: 1, OptionLength: 1})
	ip.Options = append(ip.Options, IPv4Option{OptionType: 0, OptionLength: 1})
	length := ip.getIPv4OptionSize()
	if length != 4 {
		t.Fatalf("After padding, the list should have 4 length.  Actual %d", length)
	}
}

// Test the function getIPv4OptionSize when the ipv4 has some non-trivial option and end of list option
func TestGetIPOptLengthOptionEndOfList(t *testing.T) {
	ip := IPv4{}
	someByte := make([]byte, 8)
	ip.Options = append(ip.Options, IPv4Option{OptionType: 2, OptionLength: 10, OptionData: someByte})
	ip.Options = append(ip.Options, IPv4Option{OptionType: 0, OptionLength: 1})
	length := ip.getIPv4OptionSize()
	if length != 12 {
		t.Fatalf("The list should have 12 length.  Actual %d", length)
	}
}
