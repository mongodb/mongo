// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"
	"fmt"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/bsonx/bsoncore"
	"go.mongodb.org/mongo-driver/x/network/result"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// IsMaster represents the isMaster command.
//
// The isMaster command is used for setting up a connection to MongoDB and
// for monitoring a MongoDB server.
//
// Since IsMaster can only be run on a connection, there is no Dispatch method.
type IsMaster struct {
	Client             bsonx.Doc
	Compressors        []string
	SaslSupportedMechs string

	err error
	res result.IsMaster
}

// Encode will encode this command into a wire message for the given server description.
func (im *IsMaster) Encode() (wiremessage.WireMessage, error) {
	cmd := bsonx.Doc{{"isMaster", bsonx.Int32(1)}}
	if im.Client != nil {
		cmd = append(cmd, bsonx.Elem{"client", bsonx.Document(im.Client)})
	}
	if im.SaslSupportedMechs != "" {
		cmd = append(cmd, bsonx.Elem{"saslSupportedMechs", bsonx.String(im.SaslSupportedMechs)})
	}

	// always send compressors even if empty slice
	array := bsonx.Arr{}
	for _, compressor := range im.Compressors {
		array = append(array, bsonx.String(compressor))
	}

	cmd = append(cmd, bsonx.Elem{"compression", bsonx.Array(array)})

	rdr, err := cmd.MarshalBSON()
	if err != nil {
		return nil, err
	}
	query := wiremessage.Query{
		MsgHeader:          wiremessage.Header{RequestID: wiremessage.NextRequestID()},
		FullCollectionName: "admin.$cmd",
		Flags:              wiremessage.SlaveOK,
		NumberToReturn:     -1,
		Query:              rdr,
	}
	return query, nil
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (im *IsMaster) Decode(wm wiremessage.WireMessage) *IsMaster {
	reply, ok := wm.(wiremessage.Reply)
	if !ok {
		im.err = fmt.Errorf("unsupported response wiremessage type %T", wm)
		return im
	}
	rdr, err := decodeCommandOpReply(reply)
	if err != nil {
		im.err = err
		return im
	}
	err = bson.Unmarshal(rdr, &im.res)
	if err != nil {
		im.err = err
		return im
	}

	// Reconstructs the $clusterTime doc after decode
	if im.res.ClusterTime != nil {
		im.res.ClusterTime = bsoncore.BuildDocument(nil, bsoncore.AppendDocumentElement(nil, "$clusterTime", im.res.ClusterTime))
	}
	return im
}

// Result returns the result of a decoded wire message and server description.
func (im *IsMaster) Result() (result.IsMaster, error) {
	if im.err != nil {
		return result.IsMaster{}, im.err
	}

	return im.res, nil
}

// Err returns the error set on this command.
func (im *IsMaster) Err() error { return im.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (im *IsMaster) RoundTrip(ctx context.Context, rw wiremessage.ReadWriter) (result.IsMaster, error) {
	wm, err := im.Encode()
	if err != nil {
		return result.IsMaster{}, err
	}

	err = rw.WriteWireMessage(ctx, wm)
	if err != nil {
		return result.IsMaster{}, err
	}
	wm, err = rw.ReadWireMessage(ctx)
	if err != nil {
		return result.IsMaster{}, err
	}
	return im.Decode(wm).Result()
}
