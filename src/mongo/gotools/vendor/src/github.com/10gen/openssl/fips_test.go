package openssl_test

import (
	"testing"

	"github.com/10gen/openssl"
)

func TestSetFIPSMode(t *testing.T) {
	if !openssl.FIPSModeDefined() {
		t.Skip()
	}

	if openssl.FIPSMode() {
		t.Fatal("Expected FIPS mode to be disabled, but was enabled")
	}

	err := openssl.FIPSModeSet(true)
	if err != nil {
		t.Fatal(err)
	}

	if !openssl.FIPSMode() {
		t.Fatal("Expected FIPS mode to be enabled, but was disabled")
	}

	err = openssl.FIPSModeSet(false)
	if err != nil {
		t.Fatal(err)
	}

	if openssl.FIPSMode() {
		t.Fatal("Expected FIPS mode to be disabled, but was enabled")
	}
}
