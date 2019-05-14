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
	"go.mongodb.org/mongo-driver/mongo/readconcern"
	"go.mongodb.org/mongo-driver/mongo/readpref"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// Read represents a generic database read command.
type Read struct {
	DB          string
	Command     bsonx.Doc
	ReadPref    *readpref.ReadPref
	ReadConcern *readconcern.ReadConcern
	Clock       *session.ClusterClock
	Session     *session.Client

	result bson.Raw
	err    error
}

func (r *Read) createReadPref(serverKind description.ServerKind, topologyKind description.TopologyKind, isOpQuery bool) bsonx.Doc {
	doc := bsonx.Doc{}
	rp := r.ReadPref

	if rp == nil {
		if topologyKind == description.Single && serverKind != description.Mongos {
			return append(doc, bsonx.Elem{"mode", bsonx.String("primaryPreferred")})
		}
		return nil
	}

	switch rp.Mode() {
	case readpref.PrimaryMode:
		if serverKind == description.Mongos {
			return nil
		}
		if topologyKind == description.Single {
			return append(doc, bsonx.Elem{"mode", bsonx.String("primaryPreferred")})
		}
		doc = append(doc, bsonx.Elem{"mode", bsonx.String("primary")})
	case readpref.PrimaryPreferredMode:
		doc = append(doc, bsonx.Elem{"mode", bsonx.String("primaryPreferred")})
	case readpref.SecondaryPreferredMode:
		_, ok := r.ReadPref.MaxStaleness()
		if serverKind == description.Mongos && isOpQuery && !ok && len(r.ReadPref.TagSets()) == 0 {
			return nil
		}
		doc = append(doc, bsonx.Elem{"mode", bsonx.String("secondaryPreferred")})
	case readpref.SecondaryMode:
		doc = append(doc, bsonx.Elem{"mode", bsonx.String("secondary")})
	case readpref.NearestMode:
		doc = append(doc, bsonx.Elem{"mode", bsonx.String("nearest")})
	}

	sets := make([]bsonx.Val, 0, len(r.ReadPref.TagSets()))
	for _, ts := range r.ReadPref.TagSets() {
		if len(ts) == 0 {
			continue
		}
		set := bsonx.Doc{}
		for _, t := range ts {
			set = append(set, bsonx.Elem{t.Name, bsonx.String(t.Value)})
		}
		sets = append(sets, bsonx.Document(set))
	}
	if len(sets) > 0 {
		doc = append(doc, bsonx.Elem{"tags", bsonx.Array(sets)})
	}

	if d, ok := r.ReadPref.MaxStaleness(); ok {
		doc = append(doc, bsonx.Elem{"maxStalenessSeconds", bsonx.Int32(int32(d.Seconds()))})
	}

	return doc
}

// addReadPref will add a read preference to the query document.
//
// NOTE: This method must always return either a valid bson.Reader or an error.
func (r *Read) addReadPref(rp *readpref.ReadPref, serverKind description.ServerKind, topologyKind description.TopologyKind, query bson.Raw) (bson.Raw, error) {
	doc := r.createReadPref(serverKind, topologyKind, true)
	if doc == nil {
		return query, nil
	}

	qdoc := bsonx.Doc{}
	err := bson.Unmarshal(query, &qdoc)
	if err != nil {
		return query, err
	}
	return bsonx.Doc{
		{"$query", bsonx.Document(qdoc)},
		{"$readPreference", bsonx.Document(doc)},
	}.MarshalBSON()
}

// Encode r as OP_MSG
func (r *Read) encodeOpMsg(desc description.SelectedServer, cmd bsonx.Doc) (wiremessage.WireMessage, error) {
	msg := wiremessage.Msg{
		MsgHeader: wiremessage.Header{RequestID: wiremessage.NextRequestID()},
		Sections:  make([]wiremessage.Section, 0),
	}

	readPrefDoc := r.createReadPref(desc.Server.Kind, desc.Kind, false)
	fullDocRdr, err := opmsgAddGlobals(cmd, r.DB, readPrefDoc)
	if err != nil {
		return nil, err
	}

	// type 0 doc
	msg.Sections = append(msg.Sections, wiremessage.SectionBody{
		PayloadType: wiremessage.SingleDocument,
		Document:    fullDocRdr,
	})

	// no flags to add

	return msg, nil
}

func (r *Read) slaveOK(desc description.SelectedServer) wiremessage.QueryFlag {
	if desc.Kind == description.Single && desc.Server.Kind != description.Mongos {
		return wiremessage.SlaveOK
	}

	if r.ReadPref == nil {
		// assume primary
		return 0
	}

	if r.ReadPref.Mode() != readpref.PrimaryMode {
		return wiremessage.SlaveOK
	}

	return 0
}

// Encode c as OP_QUERY
func (r *Read) encodeOpQuery(desc description.SelectedServer, cmd bsonx.Doc) (wiremessage.WireMessage, error) {
	rdr, err := marshalCommand(cmd)
	if err != nil {
		return nil, err
	}

	if desc.Server.Kind == description.Mongos {
		rdr, err = r.addReadPref(r.ReadPref, desc.Server.Kind, desc.Kind, rdr)
		if err != nil {
			return nil, err
		}
	}

	query := wiremessage.Query{
		MsgHeader:          wiremessage.Header{RequestID: wiremessage.NextRequestID()},
		FullCollectionName: r.DB + ".$cmd",
		Flags:              r.slaveOK(desc),
		NumberToReturn:     -1,
		Query:              rdr,
	}

	return query, nil
}

func (r *Read) decodeOpMsg(wm wiremessage.WireMessage) {
	msg, ok := wm.(wiremessage.Msg)
	if !ok {
		r.err = fmt.Errorf("unsupported response wiremessage type %T", wm)
		return
	}

	r.result, r.err = decodeCommandOpMsg(msg)
}

func (r *Read) decodeOpReply(wm wiremessage.WireMessage) {
	reply, ok := wm.(wiremessage.Reply)
	if !ok {
		r.err = fmt.Errorf("unsupported response wiremessage type %T", wm)
		return
	}
	r.result, r.err = decodeCommandOpReply(reply)
}

// Encode will encode this command into a wire message for the given server description.
func (r *Read) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	cmd := r.Command.Copy()
	cmd, err := addReadConcern(cmd, desc, r.ReadConcern, r.Session)
	if err != nil {
		return nil, err
	}

	cmd, err = addSessionFields(cmd, desc, r.Session)
	if err != nil {
		return nil, err
	}

	cmd = addClusterTime(cmd, desc, r.Session, r.Clock)

	if desc.WireVersion == nil || desc.WireVersion.Max < wiremessage.OpmsgWireVersion {
		return r.encodeOpQuery(desc, cmd)
	}

	return r.encodeOpMsg(desc, cmd)
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (r *Read) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *Read {
	switch wm.(type) {
	case wiremessage.Reply:
		r.decodeOpReply(wm)
	default:
		r.decodeOpMsg(wm)
	}

	if r.err != nil {
		// decode functions set error if an invalid response document was returned or if the OK flag in the response was 0
		// if the OK flag was 0, a type Error is returned. otherwise, a special type is returned
		cerr, ok := r.err.(Error)
		if !ok {
			return r // for missing/invalid response docs, don't update cluster times
		}
		if cerr.HasErrorLabel(TransientTransactionError) {
			r.Session.ClearPinnedServer()
		}
	}

	_ = updateClusterTimes(r.Session, r.Clock, r.result)
	_ = updateOperationTime(r.Session, r.result)
	r.Session.UpdateRecoveryToken(r.result)
	return r
}

// Result returns the result of a decoded wire message and server description.
func (r *Read) Result() (bson.Raw, error) {
	if r.err != nil {
		return nil, r.err
	}

	return r.result, nil
}

// Err returns the error set on this command.
func (r *Read) Err() error {
	return r.err
}

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriter.
func (r *Read) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (bson.Raw, error) {
	wm, err := r.Encode(desc)
	if err != nil {
		return nil, err
	}

	err = rw.WriteWireMessage(ctx, wm)
	if err != nil {
		if _, ok := err.(Error); ok {
			return nil, err
		}
		// Connection errors are transient
		r.Session.ClearPinnedServer()
		return nil, Error{Message: err.Error(), Labels: []string{TransientTransactionError, NetworkError}}
	}
	wm, err = rw.ReadWireMessage(ctx)
	if err != nil {
		if _, ok := err.(Error); ok {
			return nil, err
		}
		// Connection errors are transient
		r.Session.ClearPinnedServer()
		return nil, Error{Message: err.Error(), Labels: []string{TransientTransactionError, NetworkError}}
	}

	if r.Session != nil {
		err = r.Session.UpdateUseTime()
		if err != nil {
			return nil, err
		}
	}
	return r.Decode(desc, wm).Result()
}
