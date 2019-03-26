// Copyright 2019 The GoPacket Authors. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package gopacket

import (
	"testing"
	"time"
)

func TestToDuration(t *testing.T) {
	for i, test := range []struct {
		r TimestampResolution
		d time.Duration
	}{
		{
			TimestampResolutionMillisecond,
			time.Millisecond,
		},
		{
			TimestampResolutionMicrosecond,
			time.Microsecond,
		},
		{
			TimestampResolutionNanosecond,
			time.Nanosecond,
		},
		{
			TimestampResolutionNTP,
			0, // this is not representable since it's ~0.233 nanoseconds
		},
		{
			TimestampResolution{2, -16},
			15258,
		},
		{
			TimestampResolution{2, 1},
			2 * time.Second,
		},
		{
			TimestampResolution{10, 1},
			10 * time.Second,
		},
		{
			TimestampResolution{10, 0},
			time.Second,
		},
		{
			TimestampResolution{2, 0},
			time.Second,
		},
		{
			TimestampResolution{0, 0},
			0,
		},
		{
			TimestampResolution{3, 2},
			9 * time.Second,
		},
		{
			TimestampResolution{3, -2},
			111111111,
		},
	} {
		d := test.r.ToDuration()
		if d != test.d {
			t.Errorf("%d: resolution: %s want: %d got: %d", i, test.r, test.d, d)
		}
	}
}
