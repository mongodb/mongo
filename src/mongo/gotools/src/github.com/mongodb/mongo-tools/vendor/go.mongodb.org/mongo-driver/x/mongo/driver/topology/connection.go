// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package topology

import (
	"context"
	"net"

	"strings"

	"go.mongodb.org/mongo-driver/x/network/command"
	"go.mongodb.org/mongo-driver/x/network/connection"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// sconn is a wrapper around a connection.Connection. This type is returned by
// a Server so that it can track network errors and when a non-timeout network
// error is returned, the pool on the server can be cleared.
type sconn struct {
	connection.Connection
	s  *Server
	id uint64
}

var notMasterCodes = []int32{10107, 13435}
var recoveringCodes = []int32{11600, 11602, 13436, 189, 91}

func (sc *sconn) ReadWireMessage(ctx context.Context) (wiremessage.WireMessage, error) {
	wm, err := sc.Connection.ReadWireMessage(ctx)
	if err != nil {
		sc.processErr(err)
	} else {
		e := command.DecodeError(wm)
		sc.processErr(e)
	}
	return wm, err
}

func (sc *sconn) WriteWireMessage(ctx context.Context, wm wiremessage.WireMessage) error {
	err := sc.Connection.WriteWireMessage(ctx, wm)
	sc.processErr(err)
	return err
}

func (sc *sconn) processErr(err error) {
	// Invalidate server description if not master or node recovering error occurs
	if cerr, ok := err.(command.Error); ok && (isRecoveringError(cerr) || isNotMasterError(cerr)) {
		desc := sc.s.Description()
		desc.Kind = description.Unknown
		desc.LastError = err
		// updates description to unknown
		sc.s.updateDescription(desc, false)
		sc.s.RequestImmediateCheck()
		_ = sc.s.pool.Drain()
		return
	}

	ne, ok := err.(connection.NetworkError)
	if !ok {
		return
	}

	if netErr, ok := ne.Wrapped.(net.Error); ok && netErr.Timeout() {
		return
	}
	if ne.Wrapped == context.Canceled || ne.Wrapped == context.DeadlineExceeded {
		return
	}

	desc := sc.s.Description()
	desc.Kind = description.Unknown
	desc.LastError = err
	// updates description to unknown
	sc.s.updateDescription(desc, false)
}

func isRecoveringError(err command.Error) bool {
	for _, c := range recoveringCodes {
		if c == err.Code {
			return true
		}
	}
	return strings.Contains(err.Error(), "node is recovering")
}

func isNotMasterError(err command.Error) bool {
	for _, c := range notMasterCodes {
		if c == err.Code {
			return true
		}
	}
	return strings.Contains(err.Error(), "not master")
}
