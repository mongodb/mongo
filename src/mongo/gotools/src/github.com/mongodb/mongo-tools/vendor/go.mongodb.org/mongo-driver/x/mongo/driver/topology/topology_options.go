// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package topology

import (
	"bytes"
	"strings"
	"time"

	"go.mongodb.org/mongo-driver/x/mongo/driver/auth"
	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/connection"
	"go.mongodb.org/mongo-driver/x/network/connstring"
)

// Option is a configuration option for a topology.
type Option func(*config) error

type config struct {
	mode                   MonitorMode
	replicaSetName         string
	seedList               []string
	serverOpts             []ServerOption
	cs                     connstring.ConnString
	serverSelectionTimeout time.Duration
}

func newConfig(opts ...Option) (*config, error) {
	cfg := &config{
		seedList:               []string{"localhost:27017"},
		serverSelectionTimeout: 30 * time.Second,
	}

	for _, opt := range opts {
		err := opt(cfg)
		if err != nil {
			return nil, err
		}
	}

	return cfg, nil
}

// WithConnString configures the topology using the connection string.
func WithConnString(fn func(connstring.ConnString) connstring.ConnString) Option {
	return func(c *config) error {
		cs := fn(c.cs)
		c.cs = cs

		if cs.ServerSelectionTimeoutSet {
			c.serverSelectionTimeout = cs.ServerSelectionTimeout
		}

		var connOpts []connection.Option

		if cs.AppName != "" {
			connOpts = append(connOpts, connection.WithAppName(func(string) string { return cs.AppName }))
		}

		switch cs.Connect {
		case connstring.SingleConnect:
			c.mode = SingleMode
		}

		c.seedList = cs.Hosts

		if cs.ConnectTimeout > 0 {
			c.serverOpts = append(c.serverOpts, WithHeartbeatTimeout(func(time.Duration) time.Duration { return cs.ConnectTimeout }))
			connOpts = append(connOpts, connection.WithConnectTimeout(func(time.Duration) time.Duration { return cs.ConnectTimeout }))
		}

		if cs.SocketTimeoutSet {
			connOpts = append(
				connOpts,
				connection.WithReadTimeout(func(time.Duration) time.Duration { return cs.SocketTimeout }),
				connection.WithWriteTimeout(func(time.Duration) time.Duration { return cs.SocketTimeout }),
			)
		}

		if cs.HeartbeatInterval > 0 {
			c.serverOpts = append(c.serverOpts, WithHeartbeatInterval(func(time.Duration) time.Duration { return cs.HeartbeatInterval }))
		}

		if cs.MaxConnIdleTime > 0 {
			connOpts = append(connOpts, connection.WithIdleTimeout(func(time.Duration) time.Duration { return cs.MaxConnIdleTime }))
		}

		if cs.MaxPoolSizeSet {
			c.serverOpts = append(c.serverOpts, WithMaxConnections(func(uint16) uint16 { return cs.MaxPoolSize }))
			c.serverOpts = append(c.serverOpts, WithMaxIdleConnections(func(uint16) uint16 { return cs.MaxPoolSize }))
		}

		if cs.ReplicaSet != "" {
			c.replicaSetName = cs.ReplicaSet
		}

		var x509Username string
		if cs.SSL {
			tlsConfig := connection.NewTLSConfig()

			if cs.SSLCaFileSet {
				err := tlsConfig.AddCACertFromFile(cs.SSLCaFile)
				if err != nil {
					return err
				}
			}

			if cs.SSLInsecure {
				tlsConfig.SetInsecure(true)
			}

			if cs.SSLClientCertificateKeyFileSet {
				if cs.SSLClientCertificateKeyPasswordSet && cs.SSLClientCertificateKeyPassword != nil {
					tlsConfig.SetClientCertDecryptPassword(cs.SSLClientCertificateKeyPassword)
				}
				s, err := tlsConfig.AddClientCertFromFile(cs.SSLClientCertificateKeyFile)
				if err != nil {
					return err
				}

				// The Go x509 package gives the subject with the pairs in reverse order that we want.
				pairs := strings.Split(s, ",")
				b := bytes.NewBufferString("")

				for i := len(pairs) - 1; i >= 0; i-- {
					b.WriteString(pairs[i])

					if i > 0 {
						b.WriteString(",")
					}
				}

				x509Username = b.String()
			}

			connOpts = append(connOpts, connection.WithTLSConfig(func(*connection.TLSConfig) *connection.TLSConfig { return tlsConfig }))
		}

		if cs.Username != "" || cs.AuthMechanism == auth.MongoDBX509 || cs.AuthMechanism == auth.GSSAPI {
			cred := &auth.Cred{
				Source:      "admin",
				Username:    cs.Username,
				Password:    cs.Password,
				PasswordSet: cs.PasswordSet,
				Props:       cs.AuthMechanismProperties,
			}

			if cs.AuthSource != "" {
				cred.Source = cs.AuthSource
			} else {
				switch cs.AuthMechanism {
				case auth.MongoDBX509:
					if cred.Username == "" {
						cred.Username = x509Username
					}
					fallthrough
				case auth.GSSAPI, auth.PLAIN:
					cred.Source = "$external"
				default:
					cred.Source = cs.Database
				}
			}

			authenticator, err := auth.CreateAuthenticator(cs.AuthMechanism, cred)
			if err != nil {
				return err
			}

			connOpts = append(connOpts, connection.WithHandshaker(func(h connection.Handshaker) connection.Handshaker {
				options := &auth.HandshakeOptions{
					AppName:       cs.AppName,
					Authenticator: authenticator,
					Compressors:   cs.Compressors,
				}
				if cs.AuthMechanism == "" {
					// Required for SASL mechanism negotiation during handshake
					options.DBUser = cred.Source + "." + cred.Username
				}
				return auth.Handshaker(h, options)
			}))
		} else {
			// We need to add a non-auth Handshaker to the connection options
			connOpts = append(connOpts, connection.WithHandshaker(func(h connection.Handshaker) connection.Handshaker {
				return &command.Handshake{Client: command.ClientDoc(cs.AppName), Compressors: cs.Compressors}
			}))
		}

		if len(cs.Compressors) > 0 {
			connOpts = append(connOpts, connection.WithCompressors(func(compressors []string) []string {
				return append(compressors, cs.Compressors...)
			}))

			for _, comp := range cs.Compressors {
				if comp == "zlib" {
					connOpts = append(connOpts, connection.WithZlibLevel(func(level *int) *int {
						return &cs.ZlibLevel
					}))
				}
			}

			c.serverOpts = append(c.serverOpts, WithCompressionOptions(func(opts ...string) []string {
				return append(opts, cs.Compressors...)
			}))
		}

		if len(connOpts) > 0 {
			c.serverOpts = append(c.serverOpts, WithConnectionOptions(func(opts ...connection.Option) []connection.Option {
				return append(opts, connOpts...)
			}))
		}

		return nil
	}
}

// WithMode configures the topology's monitor mode.
func WithMode(fn func(MonitorMode) MonitorMode) Option {
	return func(cfg *config) error {
		cfg.mode = fn(cfg.mode)
		return nil
	}
}

// WithReplicaSetName configures the topology's default replica set name.
func WithReplicaSetName(fn func(string) string) Option {
	return func(cfg *config) error {
		cfg.replicaSetName = fn(cfg.replicaSetName)
		return nil
	}
}

// WithSeedList configures a topology's seed list.
func WithSeedList(fn func(...string) []string) Option {
	return func(cfg *config) error {
		cfg.seedList = fn(cfg.seedList...)
		return nil
	}
}

// WithServerOptions configures a topology's server options for when a new server
// needs to be created.
func WithServerOptions(fn func(...ServerOption) []ServerOption) Option {
	return func(cfg *config) error {
		cfg.serverOpts = fn(cfg.serverOpts...)
		return nil
	}
}

// WithServerSelectionTimeout configures a topology's server selection timeout.
// A server selection timeout of 0 means there is no timeout for server selection.
func WithServerSelectionTimeout(fn func(time.Duration) time.Duration) Option {
	return func(cfg *config) error {
		cfg.serverSelectionTimeout = fn(cfg.serverSelectionTimeout)
		return nil
	}
}
