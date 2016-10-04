// Copyright 2012 Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package macs

import (
	"testing"
)

func BenchmarkCheckEthernetPrefix(b *testing.B) {
	key := [3]byte{5, 5, 5}
	for i := 0; i < b.N; i++ {
		_ = ValidMACPrefixMap[key]
	}
}
