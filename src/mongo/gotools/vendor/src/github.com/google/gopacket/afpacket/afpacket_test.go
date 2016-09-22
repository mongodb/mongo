// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

// +build linux

package afpacket

import (
	"reflect"
	"testing"
)

func TestParseOptions(t *testing.T) {
	wanted1 := defaultOpts
	wanted1.frameSize = 1 << 10
	wanted1.framesPerBlock = wanted1.blockSize / wanted1.frameSize
	for i, test := range []struct {
		opts []interface{}
		want options
		err  bool
	}{
		{opts: []interface{}{OptBlockSize(2)}, err: true},
		{opts: []interface{}{OptFrameSize(333)}, err: true},
		{opts: []interface{}{OptTPacketVersion(-3)}, err: true},
		{opts: []interface{}{OptTPacketVersion(5)}, err: true},
		{opts: []interface{}{OptFrameSize(1 << 10)}, want: wanted1},
	} {
		got, err := parseOptions(test.opts...)
		t.Logf("got: %#v\nerr: %v", got, err)
		if test.err && err == nil || !test.err && err != nil {
			t.Errorf("%d error mismatch, want error? %v.  error: %v", i, test.err, err)
		}
		if !test.err && !reflect.DeepEqual(test.want, got) {
			t.Errorf("%d opts mismatch, want\n%#v", i, test.want)
		}
	}
}
