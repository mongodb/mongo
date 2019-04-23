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
	"go.mongodb.org/mongo-driver/x/network/result"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// BuildInfo represents the buildInfo command.
//
// The buildInfo command is used for getting the build information for a
// MongoDB server.
type BuildInfo struct {
	err error
	res result.BuildInfo
}

// Encode will encode this command into a wire message for the given server description.
func (bi *BuildInfo) Encode() (wiremessage.WireMessage, error) {
	// This can probably just be a global variable that we reuse.
	cmd := bsonx.Doc{{"buildInfo", bsonx.Int32(1)}}
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
func (bi *BuildInfo) Decode(wm wiremessage.WireMessage) *BuildInfo {
	reply, ok := wm.(wiremessage.Reply)
	if !ok {
		bi.err = fmt.Errorf("unsupported response wiremessage type %T", wm)
		return bi
	}
	rdr, err := decodeCommandOpReply(reply)
	if err != nil {
		bi.err = err
		return bi
	}
	err = bson.Unmarshal(rdr, &bi.res)
	if err != nil {
		bi.err = err
		return bi
	}
	return bi
}

// Result returns the result of a decoded wire message and server description.
func (bi *BuildInfo) Result() (result.BuildInfo, error) {
	if bi.err != nil {
		return result.BuildInfo{}, bi.err
	}

	return bi.res, nil
}

// Err returns the error set on this command.
func (bi *BuildInfo) Err() error { return bi.err }

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (bi *BuildInfo) RoundTrip(ctx context.Context, rw wiremessage.ReadWriter) (result.BuildInfo, error) {
	wm, err := bi.Encode()
	if err != nil {
		return result.BuildInfo{}, err
	}

	err = rw.WriteWireMessage(ctx, wm)
	if err != nil {
		return result.BuildInfo{}, err
	}
	wm, err = rw.ReadWireMessage(ctx)
	if err != nil {
		return result.BuildInfo{}, err
	}
	return bi.Decode(wm).Result()
}
