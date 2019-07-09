// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package drivertest

import (
	"context"
	"errors"

	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/mongo/driver/address"
	"go.mongodb.org/mongo-driver/x/mongo/driver/description"
	"go.mongodb.org/mongo-driver/x/mongo/driver/wiremessage"
)

// ChannelConn implements the driver.Connection interface by reading and writing wire messages
// to a channel
type ChannelConn struct {
	WriteErr error
	Written  chan []byte
	ReadResp chan []byte
	ReadErr  chan error
	Desc     description.Server
}

// WriteWireMessage implements the driver.Connection interface.
func (c *ChannelConn) WriteWireMessage(ctx context.Context, wm []byte) error {
	select {
	case c.Written <- wm:
	default:
		c.WriteErr = errors.New("could not write wiremessage to written channel")
	}
	return c.WriteErr
}

// ReadWireMessage implements the driver.Connection interface.
func (c *ChannelConn) ReadWireMessage(ctx context.Context, dst []byte) ([]byte, error) {
	var wm []byte
	var err error
	select {
	case wm = <-c.ReadResp:
	case err = <-c.ReadErr:
	case <-ctx.Done():
	}
	return wm, err
}

// Description implements the driver.Connection interface.
func (c *ChannelConn) Description() description.Server { return c.Desc }

// Close implements the driver.Connection interface.
func (c *ChannelConn) Close() error {
	return nil
}

// ID implements the driver.Connection interface.
func (c *ChannelConn) ID() string {
	return "faked"
}

// Address implements the driver.Connection interface.
func (c *ChannelConn) Address() address.Address { return address.Address("0.0.0.0") }

// MakeReply creates an OP_REPLY wiremessage from a BSON document
func MakeReply(doc bsoncore.Document) []byte {
	var dst []byte
	idx, dst := wiremessage.AppendHeaderStart(dst, 10, 9, wiremessage.OpReply)
	dst = wiremessage.AppendReplyFlags(dst, 0)
	dst = wiremessage.AppendReplyCursorID(dst, 0)
	dst = wiremessage.AppendReplyStartingFrom(dst, 0)
	dst = wiremessage.AppendReplyNumberReturned(dst, 1)
	dst = append(dst, doc...)
	return bsoncore.UpdateLength(dst, idx, int32(len(dst[idx:])))
}
