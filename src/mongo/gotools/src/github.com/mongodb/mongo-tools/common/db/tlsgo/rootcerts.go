// Copyright (C) MongoDB, Inc. 2018-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Based on https://github.com/hashicorp/go-rootcerts by HashiCorp
// See THIRD-PARTY-NOTICES for original license terms.

// +build !darwin

package tlsgo

import (
	"crypto/x509"
)

// Stubbed for non-darwin systems. By returning nil, the Go library
// will use its own code for finding system certs.
func loadSystemCAs() (*x509.CertPool, error) {
	return nil, nil
}
