// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package connection

import (
	"bytes"
	"crypto/tls"
	"crypto/x509"
	"encoding/asn1"
	"encoding/hex"
	"encoding/pem"
	"errors"
	"fmt"
	"io/ioutil"
	"strings"
)

// TLSConfig contains options for configuring a TLS connection to the server.
type TLSConfig struct {
	*tls.Config
	clientCertPass func() string
}

// NewTLSConfig creates a new TLSConfig.
func NewTLSConfig() *TLSConfig {
	cfg := &TLSConfig{}
	cfg.Config = new(tls.Config)

	return cfg
}

// SetClientCertDecryptPassword sets a function to retrieve the decryption password
// necessary to read a certificate. This is a function instead of a string to
// provide greater flexibility when deciding how to retrieve and store the password.
func (c *TLSConfig) SetClientCertDecryptPassword(f func() string) {
	c.clientCertPass = f
}

// SetInsecure sets whether the client should verify the server's certificate
// chain and hostnames.
func (c *TLSConfig) SetInsecure(allow bool) {
	c.InsecureSkipVerify = allow
}

// AddCACertFromFile adds a root CA certificate to the configuration given a path
// to the containing file.
func (c *TLSConfig) AddCACertFromFile(file string) error {
	data, err := ioutil.ReadFile(file)
	if err != nil {
		return err
	}

	certBytes, err := loadCert(data)
	if err != nil {
		return err
	}

	cert, err := x509.ParseCertificate(certBytes)
	if err != nil {
		return err
	}

	if c.RootCAs == nil {
		c.RootCAs = x509.NewCertPool()
	}

	c.RootCAs.AddCert(cert)

	return nil
}

// AddClientCertFromFile adds a client certificate to the configuration given a path to the
// containing file and returns the certificate's subject name.
func (c *TLSConfig) AddClientCertFromFile(clientFile string) (string, error) {
	data, err := ioutil.ReadFile(clientFile)
	if err != nil {
		return "", err
	}

	var currentBlock *pem.Block
	var certBlock, certDecodedBlock, keyBlock []byte

	remaining := data
	start := 0
	for {
		currentBlock, remaining = pem.Decode(remaining)
		if currentBlock == nil {
			break
		}

		if currentBlock.Type == "CERTIFICATE" {
			certBlock = data[start : len(data)-len(remaining)]
			certDecodedBlock = currentBlock.Bytes
			start += len(certBlock)
		} else if strings.HasSuffix(currentBlock.Type, "PRIVATE KEY") {
			if c.clientCertPass != nil && x509.IsEncryptedPEMBlock(currentBlock) {
				var encoded bytes.Buffer
				buf, err := x509.DecryptPEMBlock(currentBlock, []byte(c.clientCertPass()))
				if err != nil {
					return "", err
				}

				pem.Encode(&encoded, &pem.Block{Type: currentBlock.Type, Bytes: buf})
				keyBlock = encoded.Bytes()
				start = len(data) - len(remaining)
			} else {
				keyBlock = data[start : len(data)-len(remaining)]
				start += len(keyBlock)
			}
		}
	}
	if len(certBlock) == 0 {
		return "", fmt.Errorf("failed to find CERTIFICATE")
	}
	if len(keyBlock) == 0 {
		return "", fmt.Errorf("failed to find PRIVATE KEY")
	}

	cert, err := tls.X509KeyPair(certBlock, keyBlock)
	if err != nil {
		return "", err
	}

	c.Certificates = append(c.Certificates, cert)

	// The documentation for the tls.X509KeyPair indicates that the Leaf certificate is not
	// retained.
	crt, err := x509.ParseCertificate(certDecodedBlock)
	if err != nil {
		return "", err
	}

	return x509CertSubject(crt), nil
}

func loadCert(data []byte) ([]byte, error) {
	var certBlock *pem.Block

	for certBlock == nil {
		if data == nil || len(data) == 0 {
			return nil, errors.New(".pem file must have both a CERTIFICATE and an RSA PRIVATE KEY section")
		}

		block, rest := pem.Decode(data)
		if block == nil {
			return nil, errors.New("invalid .pem file")
		}

		switch block.Type {
		case "CERTIFICATE":
			if certBlock != nil {
				return nil, errors.New("multiple CERTIFICATE sections in .pem file")
			}

			certBlock = block
		}

		data = rest
	}

	return certBlock.Bytes, nil
}

// Because the functionality to convert a pkix.Name to a string wasn't added until Go 1.10, we
// need to copy the implementation (along with the attributeTypeNames map below).
func x509CertSubject(cert *x509.Certificate) string {
	r := cert.Subject.ToRDNSequence()

	s := ""
	for i := 0; i < len(r); i++ {
		rdn := r[len(r)-1-i]
		if i > 0 {
			s += ","
		}
		for j, tv := range rdn {
			if j > 0 {
				s += "+"
			}

			oidString := tv.Type.String()
			typeName, ok := attributeTypeNames[oidString]
			if !ok {
				derBytes, err := asn1.Marshal(tv.Value)
				if err == nil {
					s += oidString + "=#" + hex.EncodeToString(derBytes)
					continue // No value escaping necessary.
				}

				typeName = oidString
			}

			valueString := fmt.Sprint(tv.Value)
			escaped := make([]rune, 0, len(valueString))

			for k, c := range valueString {
				escape := false

				switch c {
				case ',', '+', '"', '\\', '<', '>', ';':
					escape = true

				case ' ':
					escape = k == 0 || k == len(valueString)-1

				case '#':
					escape = k == 0
				}

				if escape {
					escaped = append(escaped, '\\', c)
				} else {
					escaped = append(escaped, c)
				}
			}

			s += typeName + "=" + string(escaped)
		}
	}

	return s
}

var attributeTypeNames = map[string]string{
	"2.5.4.6":  "C",
	"2.5.4.10": "O",
	"2.5.4.11": "OU",
	"2.5.4.3":  "CN",
	"2.5.4.5":  "SERIALNUMBER",
	"2.5.4.7":  "L",
	"2.5.4.8":  "ST",
	"2.5.4.9":  "STREET",
	"2.5.4.17": "POSTALCODE",
}
