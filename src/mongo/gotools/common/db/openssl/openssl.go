// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// +build ssl,!openssl_pre_1.0

// Package openssl implements connection to MongoDB over ssl.
package openssl

import (
	"fmt"
	"net"
	"time"

	"github.com/10gen/openssl"
	"github.com/mongodb/mongo-tools/common/db/kerberos"
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
)

// For connecting to the database over ssl
type SSLDBConnector struct {
	dialInfo *mgo.DialInfo
	ctx      *openssl.Ctx
}

// Configure the connector to connect to the server over ssl. Parses the
// connection string, and sets up the correct function to dial the server
// based on the ssl options passed in.
func (self *SSLDBConnector) Configure(opts options.ToolOptions) error {

	var err error
	self.ctx, err = setupCtx(opts)
	if err != nil {
		return fmt.Errorf("openssl configuration: %v", err)
	}

	var flags openssl.DialFlags
	flags = 0
	if opts.SSLAllowInvalidCert || opts.SSLAllowInvalidHost {
		flags = openssl.InsecureSkipHostVerification
	}
	// create the dialer func that will be used to connect
	dialer := func(addr *mgo.ServerAddr) (net.Conn, error) {
		conn, err := openssl.Dial("tcp", addr.String(), self.ctx, flags)
		if err != nil {
			// mgo discards dialer errors so log it now
			log.Logvf(log.Always, "error dialing %v: %v", addr.String(), err)
			return nil, err
		}
		// enable TCP keepalive
		err = util.EnableTCPKeepAlive(conn.UnderlyingConn(), time.Duration(opts.TCPKeepAliveSeconds)*time.Second)
		if err != nil {
			// mgo discards dialer errors so log it now
			log.Logvf(log.Always, "error enabling TCP keepalive on connection to %v: %v", addr.String(), err)
			conn.Close()
			return nil, err
		}
		return conn, nil
	}

	timeout := time.Duration(opts.Timeout) * time.Second

	// set up the dial info
	self.dialInfo = &mgo.DialInfo{
		Timeout:        timeout,
		Direct:         opts.Direct,
		ReplicaSetName: opts.ReplicaSetName,
		DialServer:     dialer,
		Username:       opts.Auth.Username,
		Password:       opts.Auth.Password,
		Source:         opts.GetAuthenticationDatabase(),
		Mechanism:      opts.Auth.Mechanism,
	}

	// create or fetch the addresses to be used to connect
	if opts.URI != nil && opts.URI.ConnectionString != "" {
		self.dialInfo.Addrs = opts.URI.GetConnectionAddrs()
	} else {
		self.dialInfo.Addrs = util.CreateConnectionAddrs(opts.Host, opts.Port)
	}
	kerberos.AddKerberosOpts(opts, self.dialInfo)
	return nil

}

// Dial the server.
func (self *SSLDBConnector) GetNewSession() (*mgo.Session, error) {
	return mgo.DialWithInfo(self.dialInfo)
}

// To be handed to mgo.DialInfo for connecting to the server.
type dialerFunc func(addr *mgo.ServerAddr) (net.Conn, error)

// Handle optionally compiled SSL initialization functions (fips mode set)
type sslInitializationFunction func(options.ToolOptions) error

var sslInitializationFunctions []sslInitializationFunction

// Creates and configures an openssl.Ctx
func setupCtx(opts options.ToolOptions) (*openssl.Ctx, error) {
	var ctx *openssl.Ctx
	var err error

	for _, sslInitFunc := range sslInitializationFunctions {
		sslInitFunc(opts)
	}

	if ctx, err = openssl.NewCtxWithVersion(openssl.AnyVersion); err != nil {
		return nil, fmt.Errorf("failure creating new openssl context with "+
			"NewCtxWithVersion(AnyVersion): %v", err)
	}

	// OpAll - Activate all bug workaround options, to support buggy client SSL's.
	// NoSSLv2 - Disable SSL v2 support
	ctx.SetOptions(openssl.OpAll | openssl.NoSSLv2)

	// HIGH - Enable strong ciphers
	// !EXPORT - Disable export ciphers (40/56 bit)
	// !aNULL - Disable anonymous auth ciphers
	// @STRENGTH - Sort ciphers based on strength
	ctx.SetCipherList("HIGH:!EXPORT:!aNULL@STRENGTH")

	// add the PEM key file with the cert and private key, if specified
	if opts.SSLPEMKeyFile != "" {
		if err = ctx.UseCertificateChainFile(opts.SSLPEMKeyFile); err != nil {
			return nil, fmt.Errorf("UseCertificateChainFile: %v", err)
		}
		if opts.SSLPEMKeyPassword != "" {
			if err = ctx.UsePrivateKeyFileWithPassword(
				opts.SSLPEMKeyFile, openssl.FiletypePEM, opts.SSLPEMKeyPassword); err != nil {
				return nil, fmt.Errorf("UsePrivateKeyFile: %v", err)
			}
		} else {
			if err = ctx.UsePrivateKeyFile(opts.SSLPEMKeyFile, openssl.FiletypePEM); err != nil {
				return nil, fmt.Errorf("UsePrivateKeyFile: %v", err)
			}
		}
		// Verify that the certificate and the key go together.
		if err = ctx.CheckPrivateKey(); err != nil {
			return nil, fmt.Errorf("CheckPrivateKey: %v", err)
		}
	}

	// If renegotiation is needed, don't return from recv() or send() until it's successful.
	// Note: this is for blocking sockets only.
	ctx.SetMode(openssl.AutoRetry)

	// Disable session caching (see SERVER-10261)
	ctx.SetSessionCacheMode(openssl.SessionCacheOff)

	if opts.SSLCAFile != "" {
		calist, err := openssl.LoadClientCAFile(opts.SSLCAFile)
		if err != nil {
			return nil, fmt.Errorf("LoadClientCAFile: %v", err)
		}
		ctx.SetClientCAList(calist)
		if err = ctx.LoadVerifyLocations(opts.SSLCAFile, ""); err != nil {
			return nil, fmt.Errorf("LoadVerifyLocations: %v", err)
		}
	} else {
		err = ctx.SetupSystemCA()
		if err != nil {
			return nil, fmt.Errorf("Error setting up system certificate authority: %v", err)
		}
	}

	var verifyOption openssl.VerifyOptions
	if opts.SSLAllowInvalidCert {
		verifyOption = openssl.VerifyNone
	} else {
		verifyOption = openssl.VerifyPeer
	}
	ctx.SetVerify(verifyOption, nil)

	if opts.SSLCRLFile != "" {
		store := ctx.GetCertificateStore()
		store.SetFlags(openssl.CRLCheck)
		lookup, err := store.AddLookup(openssl.X509LookupFile())
		if err != nil {
			return nil, fmt.Errorf("AddLookup(X509LookupFile()): %v", err)
		}
		lookup.LoadCRLFile(opts.SSLCRLFile)
	}

	return ctx, nil
}
