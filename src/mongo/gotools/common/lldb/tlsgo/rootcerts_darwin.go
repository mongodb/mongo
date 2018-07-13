// Copyright (C) MongoDB, Inc. 2018-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Based on https://github.com/hashicorp/go-rootcerts by HashiCorp
// See THIRD-PARTY-NOTICES for original license terms.

package tlsgo

import (
	"crypto/x509"
	"os/exec"
	"os/user"
	"path"
)

// loadSystemCAs has special behavior on Darwin systems to work around
// bugs loading certs from keychains.  See this GitHub issues query:
// https://github.com/golang/go/issues?utf8=%E2%9C%93&q=is%3Aissue+darwin+keychain
func loadSystemCAs() (*x509.CertPool, error) {
	pool := x509.NewCertPool()

	for _, keychain := range certKeychains() {
		err := addCertsFromKeychain(pool, keychain)
		if err != nil {
			return nil, err
		}
	}

	return pool, nil
}

func addCertsFromKeychain(pool *x509.CertPool, keychain string) error {
	cmd := exec.Command("/usr/bin/security", "find-certificate", "-a", "-p", keychain)
	data, err := cmd.Output()
	if err != nil {
		return err
	}

	pool.AppendCertsFromPEM(data)

	return nil
}

func certKeychains() []string {
	keychains := []string{
		"/System/Library/Keychains/SystemRootCertificates.keychain",
		"/Library/Keychains/System.keychain",
	}
	user, err := user.Current()
	if err == nil {
		loginKeychain := path.Join(user.HomeDir, "Library", "Keychains", "login.keychain")
		keychains = append(keychains, loginKeychain)
	}
	return keychains
}
