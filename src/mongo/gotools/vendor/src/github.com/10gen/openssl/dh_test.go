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
	"bytes"
	"testing"
)

func TestECDH(t *testing.T) {
	t.Parallel()
	if !HasECDH() {
		t.Skip("ECDH not available")
	}

	myKey, err := GenerateECKey(Prime256v1)
	if err != nil {
		t.Fatal(err)
	}
	peerKey, err := GenerateECKey(Prime256v1)
	if err != nil {
		t.Fatal(err)
	}

	mySecret, err := DeriveSharedSecret(myKey, peerKey)
	if err != nil {
		t.Fatal(err)
	}
	theirSecret, err := DeriveSharedSecret(peerKey, myKey)
	if err != nil {
		t.Fatal(err)
	}

	if bytes.Compare(mySecret, theirSecret) != 0 {
		t.Fatal("shared secrets are different")
	}
}
