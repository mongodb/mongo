// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"context"
	"fmt"

	"errors"

	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/mongo/writeconcern"
	"go.mongodb.org/mongo-driver/x/bsonx"
	"go.mongodb.org/mongo-driver/x/mongo/driverlegacy/session"
	"go.mongodb.org/mongo-driver/x/network/description"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// Write represents a generic write database command.
// This can be used to send arbitrary write commands to the database.
type Write struct {
	DB           string
	Command      bsonx.Doc
	WriteConcern *writeconcern.WriteConcern
	Clock        *session.ClusterClock
	Session      *session.Client

	result bson.Raw
	err    error
}

// Encode c as OP_MSG
func (w *Write) encodeOpMsg(desc description.SelectedServer, cmd bsonx.Doc) (wiremessage.WireMessage, error) {
	var arr bsonx.Arr
	var identifier string

	cmd, arr, identifier = opmsgRemoveArray(cmd)

	msg := wiremessage.Msg{
		MsgHeader: wiremessage.Header{RequestID: wiremessage.NextRequestID()},
		Sections:  make([]wiremessage.Section, 0),
	}

	fullDocRdr, err := opmsgAddGlobals(cmd, w.DB, nil)
	if err != nil {
		return nil, err
	}

	// type 0 doc
	msg.Sections = append(msg.Sections, wiremessage.SectionBody{
		PayloadType: wiremessage.SingleDocument,
		Document:    fullDocRdr,
	})

	// type 1 doc
	if identifier != "" {
		docSequence, err := opmsgCreateDocSequence(arr, identifier)
		if err != nil {
			return nil, err
		}

		msg.Sections = append(msg.Sections, docSequence)
	}

	// flags
	if !writeconcern.AckWrite(w.WriteConcern) {
		msg.FlagBits |= wiremessage.MoreToCome
	}

	return msg, nil
}

// Encode w as OP_QUERY
func (w *Write) encodeOpQuery(desc description.SelectedServer, cmd bsonx.Doc) (wiremessage.WireMessage, error) {
	rdr, err := marshalCommand(cmd)
	if err != nil {
		return nil, err
	}

	query := wiremessage.Query{
		MsgHeader:          wiremessage.Header{RequestID: wiremessage.NextRequestID()},
		FullCollectionName: w.DB + ".$cmd",
		Flags:              w.slaveOK(desc),
		NumberToReturn:     -1,
		Query:              rdr,
	}

	return query, nil
}

func (w *Write) slaveOK(desc description.SelectedServer) wiremessage.QueryFlag {
	if desc.Kind == description.Single && desc.Server.Kind != description.Mongos {
		return wiremessage.SlaveOK
	}

	return 0
}

func (w *Write) decodeOpReply(wm wiremessage.WireMessage) {
	reply, ok := wm.(wiremessage.Reply)
	if !ok {
		w.err = fmt.Errorf("unsupported response wiremessage type %T", wm)
		return
	}
	w.result, w.err = decodeCommandOpReply(reply)
}

func (w *Write) decodeOpMsg(wm wiremessage.WireMessage) {
	msg, ok := wm.(wiremessage.Msg)
	if !ok {
		w.err = fmt.Errorf("unsupported response wiremessage type %T", wm)
		return
	}

	w.result, w.err = decodeCommandOpMsg(msg)
}

// Encode will encode this command into a wire message for the given server description.
func (w *Write) Encode(desc description.SelectedServer) (wiremessage.WireMessage, error) {
	cmd := w.Command.Copy()
	var err error
	if w.Session != nil && w.Session.TransactionStarting() {
		// Starting transactions have a read concern, even in writes.
		cmd, err = addReadConcern(cmd, desc, nil, w.Session)
		if err != nil {
			return nil, err
		}
	}
	cmd, err = addWriteConcern(cmd, w.WriteConcern)
	if err != nil {
		return nil, err
	}

	if !writeconcern.AckWrite(w.WriteConcern) {
		// unack write with explicit session --> raise an error
		// unack write with implicit session --> do not send session ID (implicit session shouldn't have been created
		// in the first place)

		if w.Session != nil && w.Session.SessionType == session.Explicit {
			return nil, errors.New("explicit sessions cannot be used with unacknowledged writes")
		}
	} else {
		// only encode session ID for acknowledged writes
		cmd, err = addSessionFields(cmd, desc, w.Session)
		if err != nil {
			return nil, err
		}
	}

	if w.Session != nil && w.Session.RetryWrite {
		cmd = append(cmd, bsonx.Elem{"txnNumber", bsonx.Int64(w.Session.TxnNumber)})
	}

	cmd = addClusterTime(cmd, desc, w.Session, w.Clock)

	if desc.WireVersion == nil || desc.WireVersion.Max < wiremessage.OpmsgWireVersion {
		return w.encodeOpQuery(desc, cmd)
	}

	return w.encodeOpMsg(desc, cmd)
}

// Decode will decode the wire message using the provided server description. Errors during decoding
// are deferred until either the Result or Err methods are called.
func (w *Write) Decode(desc description.SelectedServer, wm wiremessage.WireMessage) *Write {
	switch wm.(type) {
	case wiremessage.Reply:
		w.decodeOpReply(wm)
	default:
		w.decodeOpMsg(wm)
	}

	if w.err != nil {
		cerr, ok := w.err.(Error)
		if !ok {
			return w
		}
		if cerr.HasErrorLabel(TransientTransactionError) {
			w.Session.ClearPinnedServer()
		}
	}

	_ = updateClusterTimes(w.Session, w.Clock, w.result)
	w.Session.UpdateRecoveryToken(w.result)

	if writeconcern.AckWrite(w.WriteConcern) {
		// don't update session operation time for unacknowledged write
		_ = updateOperationTime(w.Session, w.result)
	}
	return w
}

// Result returns the result of a decoded wire message and server description.
func (w *Write) Result() (bson.Raw, error) {
	if w.err != nil {
		return nil, w.err
	}

	return w.result, nil
}

// Err returns the error set on this command.
func (w *Write) Err() error {
	return w.err
}

// RoundTrip handles the execution of this command using the provided wiremessage.ReadWriteCloser.
func (w *Write) RoundTrip(ctx context.Context, desc description.SelectedServer, rw wiremessage.ReadWriter) (bson.Raw, error) {
	wm, err := w.Encode(desc)
	if err != nil {
		return nil, err
	}

	err = rw.WriteWireMessage(ctx, wm)
	if err != nil {
		if _, ok := err.(Error); ok {
			return nil, err
		}
		// Connection errors are transient
		w.Session.ClearPinnedServer()
		return nil, Error{Message: err.Error(), Labels: []string{TransientTransactionError, NetworkError}}
	}

	if msg, ok := wm.(wiremessage.Msg); ok {
		// don't expect response if using OP_MSG for an unacknowledged write
		if msg.FlagBits&wiremessage.MoreToCome > 0 {
			return nil, ErrUnacknowledgedWrite
		}
	}

	wm, err = rw.ReadWireMessage(ctx)
	if err != nil {
		if _, ok := err.(Error); ok {
			return nil, err
		}
		// Connection errors are transient
		w.Session.ClearPinnedServer()
		return nil, Error{Message: err.Error(), Labels: []string{TransientTransactionError, NetworkError}}
	}

	if w.Session != nil {
		err = w.Session.UpdateUseTime()
		if err != nil {
			return nil, err
		}
	}
	return w.Decode(desc, wm).Result()
}
