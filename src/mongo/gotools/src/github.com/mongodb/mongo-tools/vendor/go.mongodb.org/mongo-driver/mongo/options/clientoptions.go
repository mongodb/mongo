// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package options // import "go.mongodb.org/mongo-driver/mongo/options"

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"fmt"
	"io/ioutil"
	"net"
	"strings"
	"time"

	"go.mongodb.org/mongo-driver/bson/bsoncodec"
	"go.mongodb.org/mongo-driver/event"
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/tag"
	"go.mongodb.org/mongo-driver/x/network/connstring"
)

// ContextDialer makes new network connections
type ContextDialer interface {
	DialContext(ctx context.Context, network, address string) (net.Conn, error)
}

// Credential holds auth options.
//
// AuthMechanism indicates the mechanism to use for authentication.
// Supported values include "SCRAM-SHA-256", "SCRAM-SHA-1", "MONGODB-CR", "PLAIN", "GSSAPI", and "MONGODB-X509".
//
// AuthMechanismProperties specifies additional configuration options which may be used by certain
// authentication mechanisms. Supported properties are:
// SERVICE_NAME: Specifies the name of the service. Defaults to mongodb.
// CANONICALIZE_HOST_NAME: If true, tells the driver to canonicalize the given hostname. Defaults to false. This
// property may not be used on Linux and Darwin systems and may not be used at the same time as SERVICE_HOST.
// SERVICE_REALM: Specifies the realm of the service.
// SERVICE_HOST: Specifies a hostname for GSSAPI authentication if it is different from the server's address. For
// authentication mechanisms besides GSSAPI, this property is ignored.
//
// AuthSource specifies the database to authenticate against.
//
// Username specifies the username that will be authenticated.
//
// Password specifies the password used for authentication.
//
// PasswordSet specifies if the password is actually set, since an empty password is a valid password.
type Credential struct {
	AuthMechanism           string
	AuthMechanismProperties map[string]string
	AuthSource              string
	Username                string
	Password                string
	PasswordSet             bool
}

// ClientOptions represents all possible options to configure a client.
type ClientOptions struct {
	AppName                *string
	Auth                   *Credential
	ConnectTimeout         *time.Duration
	Compressors            []string
	Dialer                 ContextDialer
	HeartbeatInterval      *time.Duration
	Hosts                  []string
	LocalThreshold         *time.Duration
	MaxConnIdleTime        *time.Duration
	MaxPoolSize            *uint16
	Monitor                *event.CommandMonitor
	ReadConcern            *readconcern.ReadConcern
	ReadPreference         *readpref.ReadPref
	Registry               *bsoncodec.Registry
	ReplicaSet             *string
	RetryWrites            *bool
	ServerSelectionTimeout *time.Duration
	Direct                 *bool
	SocketTimeout          *time.Duration
	TLSConfig              *tls.Config
	WriteConcern           *writeconcern.WriteConcern
	ZlibLevel              *int

	err error

	// Adds an option for internal use only and should not be set. This option is deprecated and is
	// not part of the stability guarantee. It may be removed in the future.
	AuthenticateToAnything *bool
}

// Client creates a new ClientOptions instance.
func Client() *ClientOptions {
	return new(ClientOptions)
}

// Validate validates the client options. This method will return the first error found.
func (c *ClientOptions) Validate() error { return c.err }

// ApplyURI parses the provided connection string and sets the values and options accordingly.
//
// Errors that occur in this method can be retrieved by calling Validate.
//
// If the URI contains ssl=true this method will overwrite TLSConfig, even if there aren't any other
// tls options specified.
func (c *ClientOptions) ApplyURI(uri string) *ClientOptions {
	if c.err != nil {
		return c
	}

	cs, err := connstring.Parse(uri)
	if err != nil {
		c.err = err
		return c
	}

	if cs.AppName != "" {
		c.AppName = &cs.AppName
	}

	if cs.AuthMechanism != "" || cs.AuthMechanismProperties != nil || cs.AuthSource != "admin" ||
		cs.Username != "" || cs.PasswordSet {
		c.Auth = &Credential{
			AuthMechanism:           cs.AuthMechanism,
			AuthMechanismProperties: cs.AuthMechanismProperties,
			AuthSource:              cs.AuthSource,
			Username:                cs.Username,
			Password:                cs.Password,
			PasswordSet:             cs.PasswordSet,
		}
	}

	if cs.ConnectSet {
		direct := cs.Connect == connstring.SingleConnect
		c.Direct = &direct
	}

	if cs.ConnectTimeoutSet {
		c.ConnectTimeout = &cs.ConnectTimeout
	}

	if len(cs.Compressors) > 0 {
		c.Compressors = cs.Compressors
	}

	if cs.HeartbeatIntervalSet {
		c.HeartbeatInterval = &cs.HeartbeatInterval
	}

	c.Hosts = cs.Hosts

	if cs.LocalThresholdSet {
		c.LocalThreshold = &cs.LocalThreshold
	}

	if cs.MaxConnIdleTimeSet {
		c.MaxConnIdleTime = &cs.MaxConnIdleTime
	}

	if cs.MaxPoolSizeSet {
		c.MaxPoolSize = &cs.MaxPoolSize
	}

	if cs.ReadConcernLevel != "" {
		c.ReadConcern = readconcern.New(readconcern.Level(cs.ReadConcernLevel))
	}

	if cs.ReadPreference != "" || len(cs.ReadPreferenceTagSets) > 0 || cs.MaxStalenessSet {
		opts := make([]readpref.Option, 0, 1)

		tagSets := tag.NewTagSetsFromMaps(cs.ReadPreferenceTagSets)
		if len(tagSets) > 0 {
			opts = append(opts, readpref.WithTagSets(tagSets...))
		}

		if cs.MaxStaleness != 0 {
			opts = append(opts, readpref.WithMaxStaleness(cs.MaxStaleness))
		}

		mode, err := readpref.ModeFromString(cs.ReadPreference)
		if err != nil {
			c.err = err
			return c
		}

		c.ReadPreference, c.err = readpref.New(mode, opts...)
		if c.err != nil {
			return c
		}
	}

	if cs.RetryWritesSet {
		c.RetryWrites = &cs.RetryWrites
	}

	if cs.ReplicaSet != "" {
		c.ReplicaSet = &cs.ReplicaSet
	}

	if cs.ServerSelectionTimeoutSet {
		c.ServerSelectionTimeout = &cs.ServerSelectionTimeout
	}

	if cs.SocketTimeoutSet {
		c.SocketTimeout = &cs.SocketTimeout
	}

	if cs.SSL {
		tlsConfig := new(tls.Config)

		if cs.SSLCaFileSet {
			c.err = addCACertFromFile(tlsConfig, cs.SSLCaFile)
			if c.err != nil {
				return c
			}
		}

		if cs.SSLInsecure {
			tlsConfig.InsecureSkipVerify = true
		}

		if cs.SSLClientCertificateKeyFileSet {
			var keyPasswd string
			if cs.SSLClientCertificateKeyPasswordSet && cs.SSLClientCertificateKeyPassword != nil {
				keyPasswd = cs.SSLClientCertificateKeyPassword()
			}
			s, err := addClientCertFromFile(tlsConfig, cs.SSLClientCertificateKeyFile, keyPasswd)
			if err != nil {
				c.err = err
				return c
			}

			// If a username wasn't specified, add one from the certificate.
			if c.Auth != nil && strings.ToLower(c.Auth.AuthMechanism) == "mongodb-x509" && c.Auth.Username == "" {
				// The Go x509 package gives the subject with the pairs in reverse order that we want.
				pairs := strings.Split(s, ",")
				for left, right := 0, len(pairs)-1; left < right; left, right = left+1, right-1 {
					pairs[left], pairs[right] = pairs[right], pairs[left]
				}
				c.Auth.Username = strings.Join(pairs, ",")
			}
		}

		c.TLSConfig = tlsConfig
	}

	if cs.JSet || cs.WString != "" || cs.WNumberSet || cs.WTimeoutSet {
		opts := make([]writeconcern.Option, 0, 1)

		if len(cs.WString) > 0 {
			opts = append(opts, writeconcern.WTagSet(cs.WString))
		} else if cs.WNumberSet {
			opts = append(opts, writeconcern.W(cs.WNumber))
		}

		if cs.JSet {
			opts = append(opts, writeconcern.J(cs.J))
		}

		if cs.WTimeoutSet {
			opts = append(opts, writeconcern.WTimeout(cs.WTimeout))
		}

		c.WriteConcern = writeconcern.New(opts...)
	}

	if cs.ZlibLevelSet {
		c.ZlibLevel = &cs.ZlibLevel
	}

	return c
}

// SetAppName specifies the client application name. This value is used by MongoDB when it logs
// connection information and profile information, such as slow queries.
func (c *ClientOptions) SetAppName(s string) *ClientOptions {
	c.AppName = &s
	return c
}

// SetAuth sets the authentication options.
func (c *ClientOptions) SetAuth(auth Credential) *ClientOptions {
	c.Auth = &auth
	return c
}

// SetCompressors sets the compressors that can be used when communicating with a server.
func (c *ClientOptions) SetCompressors(comps []string) *ClientOptions {
	c.Compressors = comps

	return c
}

// SetConnectTimeout specifies the timeout for an initial connection to a server.
// If a custom Dialer is used, this method won't be set and the user is
// responsible for setting the ConnectTimeout for connections on the dialer
// themselves.
func (c *ClientOptions) SetConnectTimeout(d time.Duration) *ClientOptions {
	c.ConnectTimeout = &d
	return c
}

// SetDialer specifies a custom dialer used to dial new connections to a server.
// If a custom dialer is not set, a net.Dialer with a 300 second keepalive time will be used by default.
func (c *ClientOptions) SetDialer(d ContextDialer) *ClientOptions {
	c.Dialer = d
	return c
}

// SetDirect specifies whether the driver should connect directly to the server instead of
// auto-discovering other servers in the cluster.
func (c *ClientOptions) SetDirect(b bool) *ClientOptions {
	c.Direct = &b
	return c
}

// SetHeartbeatInterval specifies the interval to wait between server monitoring checks.
func (c *ClientOptions) SetHeartbeatInterval(d time.Duration) *ClientOptions {
	c.HeartbeatInterval = &d
	return c
}

// SetHosts specifies the initial list of addresses from which to discover the rest of the cluster.
func (c *ClientOptions) SetHosts(s []string) *ClientOptions {
	c.Hosts = s
	return c
}

// SetLocalThreshold specifies how far to distribute queries, beyond the server with the fastest
// round-trip time. If a server's roundtrip time is more than LocalThreshold slower than the
// the fastest, the driver will not send queries to that server.
func (c *ClientOptions) SetLocalThreshold(d time.Duration) *ClientOptions {
	c.LocalThreshold = &d
	return c
}

// SetMaxConnIdleTime specifies the maximum number of milliseconds that a connection can remain idle
// in a connection pool before being removed and closed.
func (c *ClientOptions) SetMaxConnIdleTime(d time.Duration) *ClientOptions {
	c.MaxConnIdleTime = &d
	return c
}

// SetMaxPoolSize specifies the max size of a server's connection pool.
func (c *ClientOptions) SetMaxPoolSize(u uint16) *ClientOptions {
	c.MaxPoolSize = &u
	return c
}

// SetMonitor specifies a command monitor used to see commands for a client.
func (c *ClientOptions) SetMonitor(m *event.CommandMonitor) *ClientOptions {
	c.Monitor = m
	return c
}

// SetReadConcern specifies the read concern.
func (c *ClientOptions) SetReadConcern(rc *readconcern.ReadConcern) *ClientOptions {
	c.ReadConcern = rc

	return c
}

// SetReadPreference specifies the read preference.
func (c *ClientOptions) SetReadPreference(rp *readpref.ReadPref) *ClientOptions {
	c.ReadPreference = rp

	return c
}

// SetRegistry specifies the bsoncodec.Registry.
func (c *ClientOptions) SetRegistry(registry *bsoncodec.Registry) *ClientOptions {
	c.Registry = registry
	return c
}

// SetReplicaSet specifies the name of the replica set of the cluster.
func (c *ClientOptions) SetReplicaSet(s string) *ClientOptions {
	c.ReplicaSet = &s
	return c
}

// SetRetryWrites specifies whether the client has retryable writes enabled.
func (c *ClientOptions) SetRetryWrites(b bool) *ClientOptions {
	c.RetryWrites = &b

	return c
}

// SetServerSelectionTimeout specifies a timeout in milliseconds to block for server selection.
func (c *ClientOptions) SetServerSelectionTimeout(d time.Duration) *ClientOptions {
	c.ServerSelectionTimeout = &d
	return c
}

// SetSocketTimeout specifies the time in milliseconds to attempt to send or receive on a socket
// before the attempt times out.
func (c *ClientOptions) SetSocketTimeout(d time.Duration) *ClientOptions {
	c.SocketTimeout = &d
	return c
}

// SetTLSConfig sets the tls.Config.
func (c *ClientOptions) SetTLSConfig(cfg *tls.Config) *ClientOptions {
	c.TLSConfig = cfg
	return c
}

// SetWriteConcern sets the write concern.
func (c *ClientOptions) SetWriteConcern(wc *writeconcern.WriteConcern) *ClientOptions {
	c.WriteConcern = wc

	return c
}

// SetZlibLevel sets the level for the zlib compressor.
func (c *ClientOptions) SetZlibLevel(level int) *ClientOptions {
	c.ZlibLevel = &level

	return c
}

// MergeClientOptions combines the given connstring and *ClientOptions into a single *ClientOptions in a last one wins
// fashion. The given connstring will be used for the default options, which can be overwritten using the given
// *ClientOptions.
func MergeClientOptions(opts ...*ClientOptions) *ClientOptions {
	c := Client()

	for _, opt := range opts {
		if opt == nil {
			continue
		}

		if opt.Dialer != nil {
			c.Dialer = opt.Dialer
		}
		if opt.AppName != nil {
			c.AppName = opt.AppName
		}
		if opt.Auth != nil {
			c.Auth = opt.Auth
		}
		if opt.AuthenticateToAnything != nil {
			c.AuthenticateToAnything = opt.AuthenticateToAnything
		}
		if opt.Compressors != nil {
			c.Compressors = opt.Compressors
		}
		if opt.ConnectTimeout != nil {
			c.ConnectTimeout = opt.ConnectTimeout
		}
		if opt.HeartbeatInterval != nil {
			c.HeartbeatInterval = opt.HeartbeatInterval
		}
		if len(opt.Hosts) > 0 {
			c.Hosts = opt.Hosts
		}
		if opt.LocalThreshold != nil {
			c.LocalThreshold = opt.LocalThreshold
		}
		if opt.MaxConnIdleTime != nil {
			c.MaxConnIdleTime = opt.MaxConnIdleTime
		}
		if opt.MaxPoolSize != nil {
			c.MaxPoolSize = opt.MaxPoolSize
		}
		if opt.Monitor != nil {
			c.Monitor = opt.Monitor
		}
		if opt.ReadConcern != nil {
			c.ReadConcern = opt.ReadConcern
		}
		if opt.ReadPreference != nil {
			c.ReadPreference = opt.ReadPreference
		}
		if opt.Registry != nil {
			c.Registry = opt.Registry
		}
		if opt.ReplicaSet != nil {
			c.ReplicaSet = opt.ReplicaSet
		}
		if opt.RetryWrites != nil {
			c.RetryWrites = opt.RetryWrites
		}
		if opt.ServerSelectionTimeout != nil {
			c.ServerSelectionTimeout = opt.ServerSelectionTimeout
		}
		if opt.Direct != nil {
			c.Direct = opt.Direct
		}
		if opt.SocketTimeout != nil {
			c.SocketTimeout = opt.SocketTimeout
		}
		if opt.TLSConfig != nil {
			c.TLSConfig = opt.TLSConfig
		}
		if opt.WriteConcern != nil {
			c.WriteConcern = opt.WriteConcern
		}
		if opt.ZlibLevel != nil {
			c.ZlibLevel = opt.ZlibLevel
		}
	}

	return c
}

// addCACertFromFile adds a root CA certificate to the configuration given a path
// to the containing file.
func addCACertFromFile(cfg *tls.Config, file string) error {
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

	if cfg.RootCAs == nil {
		cfg.RootCAs = x509.NewCertPool()
	}

	cfg.RootCAs.AddCert(cert)

	return nil
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

// addClientCertFromFile adds a client certificate to the configuration given a path to the
// containing file and returns the certificate's subject name.
func addClientCertFromFile(cfg *tls.Config, clientFile, keyPasswd string) (string, error) {
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
			if keyPasswd != "" && x509.IsEncryptedPEMBlock(currentBlock) {
				var encoded bytes.Buffer
				buf, err := x509.DecryptPEMBlock(currentBlock, []byte(keyPasswd))
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

	cfg.Certificates = append(cfg.Certificates, cert)

	// The documentation for the tls.X509KeyPair indicates that the Leaf certificate is not
	// retained.
	crt, err := x509.ParseCertificate(certDecodedBlock)
	if err != nil {
		return "", err
	}

	return x509CertSubject(crt), nil
}
