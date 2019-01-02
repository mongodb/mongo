// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package tlsgo implements connection to MongoDB with Go native TLS.
package tlsgo

import (
	"crypto/tls"
	"fmt"
	"net"
	"strings"
	"time"

	"github.com/mongodb/mongo-tools/common/db/kerberos"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
)

// TLSDBConnector makes a connection to the database with Go native TLS.
type TLSDBConnector struct {
	dialInfo *mgo.DialInfo
	config   *TLSConfig
}

// Configure the connector to connect to the server over ssl. Sets up the
// correct function to dial the server based on the ssl options passed in.
func (c *TLSDBConnector) Configure(opts options.ToolOptions) error {
	if opts.SSLFipsMode {
		return fmt.Errorf("FIPS mode not supported")
	}

	if opts.SSLCRLFile != "" {
		return fmt.Errorf("CRL files are not supported on this platform")
	}

	c.config = NewTLSConfig()

	if opts.SSLAllowInvalidCert || opts.SSLAllowInvalidHost {
		c.config.SetInsecure(true)
	}

	if opts.SSLPEMKeyFile != "" {
		subject, err := c.config.AddClientCertFromFile(opts.SSLPEMKeyFile, opts.SSLPEMKeyPassword)
		if err != nil {
			return err
		}
		if opts.Auth.Mechanism == "MONGODB-X509" && opts.Auth.Username == "" {
			opts.Auth.Username = subject
		}
	}

	if opts.SSLCAFile != "" {
		c.config.AddCaCertFromFile(opts.SSLCAFile)
	}

	// set up the dial info
	c.dialInfo = &mgo.DialInfo{
		Timeout:        time.Duration(opts.Timeout) * time.Second,
		Direct:         opts.Direct,
		ReplicaSetName: opts.ReplicaSetName,
		DialServer:     c.makeDialer(opts),
		Username:       opts.Auth.Username,
		Password:       opts.Auth.Password,
		Source:         opts.GetAuthenticationDatabase(),
		Mechanism:      opts.Auth.Mechanism,
	}

	// create or fetch the addresses to be used to connect
	if opts.URI != nil && opts.URI.ConnectionString != "" {
		c.dialInfo.Addrs = opts.URI.GetConnectionAddrs()
	} else {
		c.dialInfo.Addrs = util.CreateConnectionAddrs(opts.Host, opts.Port)
	}
	kerberos.AddKerberosOpts(opts, c.dialInfo)
	return nil
}

// GetNewSession dials the server.
func (c *TLSDBConnector) GetNewSession() (*mgo.Session, error) {
	return mgo.DialWithInfo(c.dialInfo)
}

// To be handed to mgo.DialInfo for connecting to the server.
type dialerFunc func(addr *mgo.ServerAddr) (net.Conn, error)

func (c *TLSDBConnector) makeDialer(opts options.ToolOptions) dialerFunc {
	return func(addr *mgo.ServerAddr) (net.Conn, error) {
		address := addr.String()
		conn, err := net.Dial("tcp", address)
		if err != nil {
			// mgo discards dialer errors so log it now
			log.Logvf(log.Always, "error dialing %v: %v", address, err)
			return nil, err
		}
		// enable TCP keepalive
		err = util.EnableTCPKeepAlive(conn, time.Duration(opts.TCPKeepAliveSeconds)*time.Second)
		if err != nil {
			// mgo discards dialer errors so log it now
			log.Logvf(log.Always, "error enabling TCP keepalive on connection to %v: %v", address, err)
			conn.Close()
			return nil, err
		}

		tlsConfig, err := c.config.MakeConfig()
		if err != nil {
			return nil, err
		}

		if !tlsConfig.InsecureSkipVerify {
			colonPos := strings.LastIndex(address, ":")
			if colonPos == -1 {
				colonPos = len(address)
			}

			hostname := address[:colonPos]
			tlsConfig.ServerName = hostname
		}

		client := tls.Client(conn, tlsConfig)
		err = client.Handshake()
		if err != nil {
			// mgo discards dialer errors so log it now
			log.Logvf(log.Always, "error doing TLS handshake with %v: %v", address, err)
			client.Close()
			return nil, err
		}

		return client, nil
	}
}
