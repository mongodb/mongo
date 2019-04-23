// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"
	"runtime"

	"go.mongodb.org/mongo-driver/version"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/network/address"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/result"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// Handshake represents a generic MongoDB Handshake. It calls isMaster and
// buildInfo.
//
// The isMaster and buildInfo commands are used to build a server description.
type Handshake struct {
	Client             bsonx.Doc
	Compressors        []string
	SaslSupportedMechs string

	ismstr result.IsMaster
	err    error
}

// Encode will encode the handshake commands into a wire message containing isMaster
func (h *Handshake) Encode() (wiremessage.WireMessage, error) {
	var wm wiremessage.WireMessage
	ismstr, err := (&IsMaster{
		Client:             h.Client,
		Compressors:        h.Compressors,
		SaslSupportedMechs: h.SaslSupportedMechs,
	}).Encode()
	if err != nil {
		return wm, err
	}

	wm = ismstr
	return wm, nil
}

// Decode will decode the wire messages.
// Errors during decoding are deferred until either the Result or Err methods
// are called.
func (h *Handshake) Decode(wm wiremessage.WireMessage) *Handshake {
	h.ismstr, h.err = (&IsMaster{}).Decode(wm).Result()
	if h.err != nil {
		return h
	}
	return h
}

// Result returns the result of decoded wire messages.
func (h *Handshake) Result(addr address.Address) (description.Server, error) {
	if h.err != nil {
		return description.Server{}, h.err
	}
	return description.NewServer(addr, h.ismstr), nil
}

// Err returns the error set on this Handshake.
func (h *Handshake) Err() error { return h.err }

// Handshake implements the connection.Handshaker interface. It is identical
// to the RoundTrip methods on other types in this package. It will execute
// the isMaster command.
func (h *Handshake) Handshake(ctx context.Context, addr address.Address, rw wiremessage.ReadWriter) (description.Server, error) {
	wm, err := h.Encode()
	if err != nil {
		return description.Server{}, err
	}

	err = rw.WriteWireMessage(ctx, wm)
	if err != nil {
		return description.Server{}, err
	}

	wm, err = rw.ReadWireMessage(ctx)
	if err != nil {
		return description.Server{}, err
	}
	return h.Decode(wm).Result(addr)
}

// ClientDoc creates a client information document for use in an isMaster
// command.
func ClientDoc(app string) bsonx.Doc {
	doc := bsonx.Doc{
		{"driver",
			bsonx.Document(bsonx.Doc{
				{"name", bsonx.String("mongo-go-driver")},
				{"version", bsonx.String(version.Driver)},
			}),
		},
		{"os",
			bsonx.Document(bsonx.Doc{
				{"type", bsonx.String(runtime.GOOS)},
				{"architecture", bsonx.String(runtime.GOARCH)},
			}),
		},
		{"platform", bsonx.String(runtime.Version())},
	}

	if app != "" {
		doc = append(doc, bsonx.Elem{"application", bsonx.Document(bsonx.Doc{{"name", bsonx.String(app)}})})
	}

	return doc
}
