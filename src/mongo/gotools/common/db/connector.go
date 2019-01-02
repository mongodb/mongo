// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package db

import (
	"net"
	"time"

	"github.com/mongodb/mongo-tools/common/db/kerberos"
	"github.com/mongodb/mongo-tools/common/options"
	"github.com/mongodb/mongo-tools/common/util"
	"gopkg.in/mgo.v2"
)

// Interface type for connecting to the database.
type DBConnector interface {
	// configure, based on the options passed in
	Configure(options.ToolOptions) error

	// dial the database and get a fresh new session
	GetNewSession() (*mgo.Session, error)
}

// Basic connector for dialing the database, with no authentication.
type VanillaDBConnector struct {
	dialInfo *mgo.DialInfo
}

// Configure sets up the db connector using the options in opts. It parses the
// connection string and then sets up the dial information using the default
// dial timeout.
func (self *VanillaDBConnector) Configure(opts options.ToolOptions) error {
	timeout := time.Duration(opts.Timeout) * time.Second

	// create the dialer func that will be used to connect
	dialer := func(addr *mgo.ServerAddr) (net.Conn, error) {
		conn, err := net.DialTimeout("tcp", addr.String(), timeout)
		if err != nil {
			return nil, err
		}
		// enable TCP keepalive
		err = util.EnableTCPKeepAlive(conn, time.Duration(opts.TCPKeepAliveSeconds)*time.Second)
		if err != nil {
			return nil, err
		}
		return conn, nil
	}

	// set up the dial info
	self.dialInfo = &mgo.DialInfo{
		Direct:         opts.Direct,
		ReplicaSetName: opts.ReplicaSetName,
		Username:       opts.Auth.Username,
		Password:       opts.Auth.Password,
		Source:         opts.GetAuthenticationDatabase(),
		Mechanism:      opts.Auth.Mechanism,
		DialServer:     dialer,
		Timeout:        timeout,
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

// GetNewSession connects to the server and returns the established session and any
// error encountered.
func (self *VanillaDBConnector) GetNewSession() (*mgo.Session, error) {
	return mgo.DialWithInfo(self.dialInfo)
}
