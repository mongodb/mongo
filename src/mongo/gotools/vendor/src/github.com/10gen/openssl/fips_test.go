package openssl_test

import (
	"testing"

	"github.com/10gen/openssl"
)

func TestSetFIPSMode(t *testing.T) {
	if !openssl.FIPSModeDefined() {
		t.Skip("OPENSSL_FIPS not defined in headers")
	}

	if openssl.FIPSMode() {
		t.Skip("FIPS mode already enabled")
	}

	err := openssl.FIPSModeSet(true)
	if err != nil {
		t.Fatal(err)
	}

	if !openssl.FIPSMode() {
		t.Fatal("Expected FIPS mode to be enabled, but was disabled")
	}

}
