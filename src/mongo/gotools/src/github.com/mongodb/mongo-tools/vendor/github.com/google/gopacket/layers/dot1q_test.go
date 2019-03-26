// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.
package layers

import (
	"fmt"
	"reflect"
	"testing"

	"github.com/google/gopacket"
)

// test harness to ensure the dot1q layer can be encoded/decoded properly
// return error if decoded data not match.
func testEncodeDecodeDot1Q(dot1Q *Dot1Q) error {
	buf := gopacket.NewSerializeBuffer()
	opts := gopacket.SerializeOptions{}
	expectedDot1Q := dot1Q

	err := dot1Q.SerializeTo(buf, opts)
	if err != nil {
		return err
	}

	newDot1q := &Dot1Q{}
	err = newDot1q.DecodeFromBytes(buf.Bytes(), gopacket.NilDecodeFeedback)
	if err != nil {
		return err
	}
	newDot1q.BaseLayer = BaseLayer{}

	if !reflect.DeepEqual(expectedDot1Q, newDot1q) {
		return fmt.Errorf("Expect %v actual %v", expectedDot1Q, newDot1q)
	}
	return nil

}

// Test to ensure what has been encode can be decoded
func TestEncodeDecodeDot1Q(t *testing.T) {
	dot1Qs := []*Dot1Q{
		&Dot1Q{
			Priority:       uint8(3),
			VLANIdentifier: uint16(30),
		},
		&Dot1Q{
			Priority:       uint8(0x07),
			DropEligible:   true,
			VLANIdentifier: uint16(0xFFF),
		},
	}

	for i, curTest := range dot1Qs {
		err := testEncodeDecodeDot1Q(curTest)
		if err != nil {
			t.Error("Error with item ", i, " with error message :", err)
		}
	}
}
