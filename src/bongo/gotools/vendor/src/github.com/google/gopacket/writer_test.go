// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package gopacket

import (
	"fmt"
	"testing"
)

func TestExponentialSizeIncreasePrepend(t *testing.T) {
	var b serializeBuffer
	for i, test := range []struct {
		prepend, size int
	}{
		{2, 2},
		{2, 4},
		{2, 8},
		{2, 8},
		{2, 16},
		{2, 16},
		{2, 16},
		{2, 16},
		{2, 32},
	} {
		b.PrependBytes(test.prepend)
		if test.size != cap(b.data) {
			t.Error(i, "size want", test.size, "got", cap(b.data))
		}
	}
	b.Clear()
	if b.start != 32 {
		t.Error(b.start)
	}
}

func TestExponentialSizeIncreaseAppend(t *testing.T) {
	var b serializeBuffer
	for i, test := range []struct {
		appnd, size int
	}{
		{2, 2},
		{2, 4},
		{2, 8},
		{2, 8},
		{2, 16},
		{2, 16},
		{2, 16},
		{2, 16},
		{2, 32},
	} {
		b.AppendBytes(test.appnd)
		if test.size != cap(b.data) {
			t.Error(i, "size want", test.size, "got", cap(b.data))
		}
	}
	b.Clear()
	if b.start != 0 {
		t.Error(b.start)
	}
}

func ExampleSerializeBuffer() {
	b := NewSerializeBuffer()
	fmt.Println("1:", b.Bytes())
	bytes, _ := b.PrependBytes(3)
	copy(bytes, []byte{1, 2, 3})
	fmt.Println("2:", b.Bytes())
	bytes, _ = b.AppendBytes(2)
	copy(bytes, []byte{4, 5})
	fmt.Println("3:", b.Bytes())
	bytes, _ = b.PrependBytes(1)
	copy(bytes, []byte{0})
	fmt.Println("4:", b.Bytes())
	bytes, _ = b.AppendBytes(3)
	copy(bytes, []byte{6, 7, 8})
	fmt.Println("5:", b.Bytes())
	b.Clear()
	fmt.Println("6:", b.Bytes())
	bytes, _ = b.PrependBytes(2)
	copy(bytes, []byte{9, 9})
	fmt.Println("7:", b.Bytes())
	// Output:
	// 1: []
	// 2: [1 2 3]
	// 3: [1 2 3 4 5]
	// 4: [0 1 2 3 4 5]
	// 5: [0 1 2 3 4 5 6 7 8]
	// 6: []
	// 7: [9 9]
}
