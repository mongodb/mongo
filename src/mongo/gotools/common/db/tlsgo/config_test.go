package tlsgo

import (
	"strings"
	"testing"
)

func TestAddClientCert(t *testing.T) {
	cases := []struct {
		Path  string
		Pass  string
		Valid bool
	}{
		{Path: "testdata/pkcs1.pem", Valid: true},
		{Path: "testdata/pkcs1-rev.pem", Valid: true},
		{Path: "testdata/pkcs1-encrypted.pem", Pass: "qwerty", Valid: true},
		{Path: "testdata/pkcs1-encrypted-rev.pem", Pass: "qwerty", Valid: true},

		{Path: "testdata/pkcs8.pem", Valid: true},
		{Path: "testdata/pkcs8-rev.pem", Valid: true},
		{Path: "testdata/pkcs8-encrypted.pem", Valid: false},
		{Path: "testdata/pkcs8-encrypted-rev.pem", Valid: false},
	}

	for _, v := range cases {
		tlsc := NewTLSConfig()
		_, err := tlsc.AddClientCertFromFile(v.Path, v.Pass)
		switch v.Valid {
		case true:
			if err != nil {
				t.Errorf("Error parsing %s: %s", v.Path, err.Error())
			}
		case false:
			if err == nil {
				t.Errorf("Expected error parsing %s but parsed OK", v.Path)
			} else if !strings.Contains(err.Error(), "encrypted private keys are not supported") {
				t.Errorf("Incorrect error for %s: %s", v.Path, err.Error())
			}
		}
	}
}
