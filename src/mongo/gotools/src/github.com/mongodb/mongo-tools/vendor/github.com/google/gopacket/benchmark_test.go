// Copyright 2012, Google, Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in the root of the source
// tree.

package gopacket

import (
	"runtime"
	"testing"
)

// A few benchmarks for figuring out exactly how fast some underlying Go
// things are.

type testError struct{}

func (t *testError) Error() string { return "abc" }

func BenchmarkTypeAssertion(b *testing.B) {
	var e error = &testError{}
	for i := 0; i < b.N; i++ {
		_, _ = e.(*testError)
	}
}

func BenchmarkMapLookup(b *testing.B) {
	m := map[LayerType]bool{
		LayerTypePayload: true,
	}
	for i := 0; i < b.N; i++ {
		_ = m[LayerTypePayload]
	}
}

func BenchmarkNilMapLookup(b *testing.B) {
	var m map[LayerType]bool
	for i := 0; i < b.N; i++ {
		_ = m[LayerTypePayload]
	}
}

func BenchmarkNilMapLookupWithNilCheck(b *testing.B) {
	var m map[LayerType]bool
	for i := 0; i < b.N; i++ {
		if m != nil {
			_ = m[LayerTypePayload]
		}
	}
}

func BenchmarkArrayLookup(b *testing.B) {
	m := make([]bool, 100)
	for i := 0; i < b.N; i++ {
		_ = m[LayerTypePayload]
	}
}

var testError1 = &testError{}
var testError2 error = testError1

func BenchmarkTypeToInterface1(b *testing.B) {
	var e error
	for i := 0; i < b.N; i++ {
		e = testError1
	}
	// Have to do someting with 'e' or the compiler complains about an unused
	// variable.
	testError2 = e
}
func BenchmarkTypeToInterface2(b *testing.B) {
	var e error
	for i := 0; i < b.N; i++ {
		e = testError2
	}
	// Have to do someting with 'e' or the compiler complains about an unused
	// variable.
	testError2 = e
}

var decodeOpts DecodeOptions

func decodeOptsByValue(_ DecodeOptions)    {}
func decodeOptsByPointer(_ *DecodeOptions) {}
func BenchmarkPassDecodeOptionsByValue(b *testing.B) {
	for i := 0; i < b.N; i++ {
		decodeOptsByValue(decodeOpts)
	}
}
func BenchmarkPassDecodeOptionsByPointer(b *testing.B) {
	for i := 0; i < b.N; i++ {
		decodeOptsByPointer(&decodeOpts)
	}
}

func BenchmarkLockOSThread(b *testing.B) {
	for i := 0; i < b.N; i++ {
		runtime.LockOSThread()
	}
}
func BenchmarkUnlockOSThread(b *testing.B) {
	for i := 0; i < b.N; i++ {
		runtime.UnlockOSThread()
	}
}
func lockUnlock() {
	runtime.LockOSThread()
	runtime.UnlockOSThread()
}
func lockDeferUnlock() {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
}
func BenchmarkLockUnlockOSThread(b *testing.B) {
	for i := 0; i < b.N; i++ {
		lockUnlock()
	}
}
func BenchmarkLockDeferUnlockOSThread(b *testing.B) {
	for i := 0; i < b.N; i++ {
		lockDeferUnlock()
	}
}

func BenchmarkUnbufferedChannel(b *testing.B) {
	ca := make(chan bool)
	cb := make(chan bool)
	defer close(ca)
	go func() {
		defer close(cb)
		for range ca {
			cb <- true
		}
	}()
	for i := 0; i < b.N; i++ {
		ca <- true
		<-cb
	}
}
func BenchmarkSmallBufferedChannel(b *testing.B) {
	ca := make(chan bool, 1)
	cb := make(chan bool, 1)
	defer close(ca)
	go func() {
		defer close(cb)
		for range ca {
			cb <- true
		}
	}()
	for i := 0; i < b.N; i++ {
		ca <- true
		<-cb
	}
}
func BenchmarkLargeBufferedChannel(b *testing.B) {
	ca := make(chan bool, 1000)
	cb := make(chan bool, 1000)
	defer close(ca)
	go func() {
		defer close(cb)
		for range ca {
			cb <- true
		}
	}()
	for i := 0; i < b.N; i++ {
		ca <- true
		<-cb
	}
}
func BenchmarkEndpointFastHashShort(b *testing.B) {
	e := Endpoint{typ: 1, len: 2}
	for i := 0; i < b.N; i++ {
		e.FastHash()
	}
}
func BenchmarkEndpointFastHashLong(b *testing.B) {
	e := Endpoint{typ: 1, len: 16}
	for i := 0; i < b.N; i++ {
		e.FastHash()
	}
}
func BenchmarkFlowFastHashShort(b *testing.B) {
	e := Flow{typ: 1, slen: 2, dlen: 2}
	for i := 0; i < b.N; i++ {
		e.FastHash()
	}
}
func BenchmarkFlowFastHashLong(b *testing.B) {
	e := Flow{typ: 1, slen: 16, dlen: 16}
	for i := 0; i < b.N; i++ {
		e.FastHash()
	}
}
