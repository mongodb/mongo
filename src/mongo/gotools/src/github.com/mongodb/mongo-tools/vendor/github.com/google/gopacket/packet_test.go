// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package gopacket

import (
	"io"
	"reflect"
	"testing"
)

type embedded struct {
	A, B int
}

type embedding struct {
	embedded
	C, D int
}

func TestDumpEmbedded(t *testing.T) {
	e := embedding{embedded: embedded{A: 1, B: 2}, C: 3, D: 4}
	if got, want := layerString(reflect.ValueOf(e), false, false), "{A=1 B=2 C=3 D=4}"; got != want {
		t.Errorf("embedded dump mismatch:\n   got: %v\n  want: %v", got, want)
	}
}

type singlePacketSource [1][]byte

func (s *singlePacketSource) ReadPacketData() ([]byte, CaptureInfo, error) {
	if (*s)[0] == nil {
		return nil, CaptureInfo{}, io.EOF
	}
	out := (*s)[0]
	(*s)[0] = nil
	return out, CaptureInfo{}, nil
}

func TestConcatPacketSources(t *testing.T) {
	sourceA := &singlePacketSource{[]byte{1}}
	sourceB := &singlePacketSource{[]byte{2}}
	sourceC := &singlePacketSource{[]byte{3}}
	concat := ConcatFinitePacketDataSources(sourceA, sourceB, sourceC)
	a, _, err := concat.ReadPacketData()
	if err != nil || len(a) != 1 || a[0] != 1 {
		t.Errorf("expected [1], got %v/%v", a, err)
	}
	b, _, err := concat.ReadPacketData()
	if err != nil || len(b) != 1 || b[0] != 2 {
		t.Errorf("expected [2], got %v/%v", b, err)
	}
	c, _, err := concat.ReadPacketData()
	if err != nil || len(c) != 1 || c[0] != 3 {
		t.Errorf("expected [3], got %v/%v", c, err)
	}
	if _, _, err := concat.ReadPacketData(); err != io.EOF {
		t.Errorf("expected io.EOF, got %v", err)
	}
}
