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
	"math/big"
	"testing"
	"time"
)

func TestCertGenerate(t *testing.T) {
	key, err := GenerateRSAKey(2048)
	if err != nil {
		t.Fatal(err)
	}
	info := &CertificateInfo{
		Serial:       big.NewInt(int64(1)),
		Issued:       0,
		Expires:      24 * time.Hour,
		Country:      "US",
		Organization: "Test",
		CommonName:   "localhost",
	}
	cert, err := NewCertificate(info, key)
	if err != nil {
		t.Fatal(err)
	}
	if err := cert.Sign(key, EVP_SHA256); err != nil {
		t.Fatal(err)
	}
}

func TestCAGenerate(t *testing.T) {
	cakey, err := GenerateRSAKey(2048)
	if err != nil {
		t.Fatal(err)
	}
	info := &CertificateInfo{
		Serial:       big.NewInt(int64(1)),
		Issued:       0,
		Expires:      24 * time.Hour,
		Country:      "US",
		Organization: "Test CA",
		CommonName:   "CA",
	}
	ca, err := NewCertificate(info, cakey)
	if err != nil {
		t.Fatal(err)
	}
	if err := ca.AddExtensions(map[NID]string{
		NID_basic_constraints:      "critical,CA:TRUE",
		NID_key_usage:              "critical,keyCertSign,cRLSign",
		NID_subject_key_identifier: "hash",
		NID_netscape_cert_type:     "sslCA",
	}); err != nil {
		t.Fatal(err)
	}
	if err := ca.Sign(cakey, EVP_SHA256); err != nil {
		t.Fatal(err)
	}
	key, err := GenerateRSAKey(2048)
	if err != nil {
		t.Fatal(err)
	}
	info = &CertificateInfo{
		Serial:       big.NewInt(int64(1)),
		Issued:       0,
		Expires:      24 * time.Hour,
		Country:      "US",
		Organization: "Test",
		CommonName:   "localhost",
	}
	cert, err := NewCertificate(info, key)
	if err != nil {
		t.Fatal(err)
	}
	if err := cert.AddExtensions(map[NID]string{
		NID_basic_constraints: "critical,CA:FALSE",
		NID_key_usage:         "keyEncipherment",
		NID_ext_key_usage:     "serverAuth",
	}); err != nil {
		t.Fatal(err)
	}
	if err := cert.SetIssuer(ca); err != nil {
		t.Fatal(err)
	}
	if err := cert.Sign(cakey, EVP_SHA256); err != nil {
		t.Fatal(err)
	}
}

func TestCertGetNameEntry(t *testing.T) {
	key, err := GenerateRSAKey(2048)
	if err != nil {
		t.Fatal(err)
	}
	info := &CertificateInfo{
		Serial:       big.NewInt(int64(1)),
		Issued:       0,
		Expires:      24 * time.Hour,
		Country:      "US",
		Organization: "Test",
		CommonName:   "localhost",
	}
	cert, err := NewCertificate(info, key)
	if err != nil {
		t.Fatal(err)
	}
	name, err := cert.GetSubjectName()
	if err != nil {
		t.Fatal(err)
	}
	entry, ok := name.GetEntry(NID_commonName)
	if !ok {
		t.Fatal("no common name")
	}
	if entry != "localhost" {
		t.Fatalf("expected localhost; got %q", entry)
	}
	entry, ok = name.GetEntry(NID_localityName)
	if ok {
		t.Fatal("did not expect a locality name")
	}
	if entry != "" {
		t.Fatalf("entry should be empty; got %q", entry)
	}
}
