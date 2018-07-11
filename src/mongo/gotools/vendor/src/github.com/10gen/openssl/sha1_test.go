// Copyright (C) 2017. See AUTHORS.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package openssl

import (
	"crypto/rand"
	"crypto/sha1"
	"io"
	"testing"
)

func TestSHA1(t *testing.T) {
	for i := 0; i < 100; i++ {
		buf := make([]byte, 10*1024-i)
		if _, err := io.ReadFull(rand.Reader, buf); err != nil {
			t.Fatal(err)
		}

		expected := sha1.Sum(buf)
		got, err := SHA1(buf)
		if err != nil {
			t.Fatal(err)
		}

		if expected != got {
			t.Fatalf("exp:%x got:%x", expected, got)
		}
	}
}

func TestSHA1Writer(t *testing.T) {
	ohash, err := NewSHA1Hash()
	if err != nil {
		t.Fatal(err)
	}
	hash := sha1.New()

	for i := 0; i < 100; i++ {
		if err := ohash.Reset(); err != nil {
			t.Fatal(err)
		}
		hash.Reset()
		buf := make([]byte, 10*1024-i)
		if _, err := io.ReadFull(rand.Reader, buf); err != nil {
			t.Fatal(err)
		}

		if _, err := ohash.Write(buf); err != nil {
			t.Fatal(err)
		}
		if _, err := hash.Write(buf); err != nil {
			t.Fatal(err)
		}

		var got, exp [20]byte

		hash.Sum(exp[:0])
		got, err := ohash.Sum()
		if err != nil {
			t.Fatal(err)
		}

		if got != exp {
			t.Fatalf("exp:%x got:%x", exp, got)
		}
	}
}

type shafunc func([]byte)

func benchmarkSHA1(b *testing.B, length int64, fn shafunc) {
	buf := make([]byte, length)
	if _, err := io.ReadFull(rand.Reader, buf); err != nil {
		b.Fatal(err)
	}
	b.SetBytes(length)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		fn(buf)
	}
}

func BenchmarkSHA1Large_openssl(b *testing.B) {
	benchmarkSHA1(b, 1024*1024, func(buf []byte) { SHA1(buf) })
}

func BenchmarkSHA1Large_stdlib(b *testing.B) {
	benchmarkSHA1(b, 1024*1024, func(buf []byte) { sha1.Sum(buf) })
}

func BenchmarkSHA1Small_openssl(b *testing.B) {
	benchmarkSHA1(b, 1, func(buf []byte) { SHA1(buf) })
}

func BenchmarkSHA1Small_stdlib(b *testing.B) {
	benchmarkSHA1(b, 1, func(buf []byte) { sha1.Sum(buf) })
}
