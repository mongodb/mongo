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

// +build !openssl_pre_1.0

package openssl

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"testing"
)

func TestSHA256HMAC(t *testing.T) {
	key := []byte("d741787cc61851af045ccd37")
	data := []byte("5912EEFD-59EC-43E3-ADB8-D5325AEC3271")

	h, err := NewHMAC(key, EVP_SHA256)
	if err != nil {
		t.Fatalf("Unable to create new HMAC: %s", err)
	}
	if _, err := h.Write(data); err != nil {
		t.Fatalf("Unable to write data into HMAC: %s", err)
	}

	var actualHMACBytes []byte
	if actualHMACBytes, err = h.Final(); err != nil {
		t.Fatalf("Error while finalizing HMAC: %s", err)
	}
	actualString := hex.EncodeToString(actualHMACBytes)

	// generate HMAC with built-in crypto lib
	mac := hmac.New(sha256.New, key)
	mac.Write(data)
	expectedString := hex.EncodeToString(mac.Sum(nil))

	if expectedString != actualString {
		t.Errorf("HMAC was incorrect: expected=%s, actual=%s", expectedString, actualString)
	}
}

func BenchmarkSHA256HMAC(b *testing.B) {
	key := []byte("d741787cc61851af045ccd37")
	data := []byte("5912EEFD-59EC-43E3-ADB8-D5325AEC3271")

	h, err := NewHMAC(key, EVP_SHA256)
	if err != nil {
		b.Fatalf("Unable to create new HMAC: %s", err)
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, err := h.Write(data); err != nil {
			b.Fatalf("Unable to write data into HMAC: %s", err)
		}

		var err error
		if _, err = h.Final(); err != nil {
			b.Fatalf("Error while finalizing HMAC: %s", err)
		}
	}
}
