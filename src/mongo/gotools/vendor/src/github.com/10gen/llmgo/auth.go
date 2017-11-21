// Copyright (C) MongoDB, Inc. 2015-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Based on gopkg.io/mgo.v2 by Gustavo Niemeyer.
// See THIRD-PARTY-NOTICES for original license terms.

package mgo

import (
	"crypto/md5"
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"sync"

	"github.com/10gen/llmgo/bson"
	"github.com/10gen/llmgo/internal/scram"
)

type authCmd struct {
	Authenticate int

	Nonce string
	User  string
	Key   string
}

type startSaslCmd struct {
	StartSASL int `bson:"startSasl"`
}

type authResult struct {
	ErrMsg string
	Ok     bool
}

type getNonceCmd struct {
	GetNonce int
}

type getNonceResult struct {
	Nonce string
	Err   string "$err"
	Code  int
}

type logoutCmd struct {
	Logout int
}

type saslCmd struct {
	Start          int    `bson:"saslStart,omitempty"`
	Continue       int    `bson:"saslContinue,omitempty"`
	ConversationId int    `bson:"conversationId,omitempty"`
	Mechanism      string `bson:"mechanism,omitempty"`
	Payload        []byte
}

type saslResult struct {
	Ok    bool `bson:"ok"`
	NotOk bool `bson:"code"` // Server <= 2.3.2 returns ok=1 & code>0 on errors (WTF?)
	Done  bool

	ConversationId int `bson:"conversationId"`
	Payload        []byte
	ErrMsg         string
}

type saslStepper interface {
	Step(serverData []byte) (clientData []byte, done bool, err error)
	Close()
}

func (socket *MongoSocket) getNonce() (nonce string, err error) {
	socket.Lock()
	for socket.cachedNonce == "" && socket.dead == nil {
		debugf("Socket %p to %s: waiting for nonce", socket, socket.addr)
		socket.gotNonce.Wait()
	}
	if socket.cachedNonce == "mongos" {
		socket.Unlock()
		return "", errors.New("Can't authenticate with mongos; see http://j.mp/mongos-auth")
	}
	debugf("Socket %p to %s: got nonce", socket, socket.addr)
	nonce, err = socket.cachedNonce, socket.dead
	socket.cachedNonce = ""
	socket.Unlock()
	if err != nil {
		nonce = ""
	}
	return
}

func (socket *MongoSocket) resetNonce() {
	debugf("Socket %p to %s: requesting a new nonce", socket, socket.addr)
	op := &QueryOp{}
	op.Query = &getNonceCmd{GetNonce: 1}
	op.Collection = "admin.$cmd"
	op.Limit = -1
	op.replyFunc = func(err error, rfl *replyFuncLegacyArgs, rfc *replyFuncCommandArgs, rfm *replyFuncMsgArgs) {
		if err != nil {
			socket.kill(errors.New("getNonce: "+err.Error()), true)
			return
		}
		result := &getNonceResult{}
		err = bson.Unmarshal(rfl.docData, &result)
		if err != nil {
			socket.kill(errors.New("Failed to unmarshal nonce: "+err.Error()), true)
			return
		}
		debugf("Socket %p to %s: nonce unmarshalled: %#v", socket, socket.addr, result)
		if result.Code == 13390 {
			// mongos doesn't yet support auth (see http://j.mp/mongos-auth)
			result.Nonce = "mongos"
		} else if result.Nonce == "" {
			var msg string
			if result.Err != "" {
				msg = fmt.Sprintf("Got an empty nonce: %s (%d)", result.Err, result.Code)
			} else {
				msg = "Got an empty nonce"
			}
			socket.kill(errors.New(msg), true)
			return
		}
		socket.Lock()
		if socket.cachedNonce != "" {
			socket.Unlock()
			panic("resetNonce: nonce already cached")
		}
		socket.cachedNonce = result.Nonce
		socket.gotNonce.Signal()
		socket.Unlock()
	}
	err := socket.Query(op)
	if err != nil {
		socket.kill(errors.New("resetNonce: "+err.Error()), true)
	}
}

func (socket *MongoSocket) Login(cred Credential) error {
	socket.Lock()
	if cred.Mechanism == "" && socket.serverInfo.MaxWireVersion >= 3 {
		cred.Mechanism = "SCRAM-SHA-1"
	}
	for _, sockCred := range socket.creds {
		if sockCred == cred {
			debugf("Socket %p to %s: login: db=%q user=%q (already logged in)", socket, socket.addr, cred.Source, cred.Username)
			socket.Unlock()
			return nil
		}
	}
	if socket.dropLogout(cred) {
		debugf("Socket %p to %s: login: db=%q user=%q (cached)", socket, socket.addr, cred.Source, cred.Username)
		socket.creds = append(socket.creds, cred)
		socket.Unlock()
		return nil
	}
	socket.Unlock()

	debugf("Socket %p to %s: login: db=%q user=%q", socket, socket.addr, cred.Source, cred.Username)

	var err error
	switch cred.Mechanism {
	case "", "MONGODB-CR", "MONGO-CR": // Name changed to MONGODB-CR in SERVER-8501.
		err = socket.loginClassic(cred)
	case "PLAIN":
		err = socket.loginPlain(cred)
	case "MONGODB-X509":
		err = socket.loginX509(cred)
	default:
		// Try SASL for everything else, if it is available.
		err = socket.loginSASL(cred)
	}

	if err != nil {
		debugf("Socket %p to %s: login error: %s", socket, socket.addr, err)
	} else {
		debugf("Socket %p to %s: login successful", socket, socket.addr)
	}
	return err
}

func (socket *MongoSocket) loginClassic(cred Credential) error {
	// Note that this only works properly because this function is
	// synchronous, which means the nonce won't get reset while we're
	// using it and any other login requests will block waiting for a
	// new nonce provided in the defer call below.
	nonce, err := socket.getNonce()
	if err != nil {
		return err
	}
	defer socket.resetNonce()

	psum := md5.New()
	psum.Write([]byte(cred.Username + ":mongo:" + cred.Password))

	ksum := md5.New()
	ksum.Write([]byte(nonce + cred.Username))
	ksum.Write([]byte(hex.EncodeToString(psum.Sum(nil))))

	key := hex.EncodeToString(ksum.Sum(nil))

	cmd := authCmd{Authenticate: 1, User: cred.Username, Nonce: nonce, Key: key}
	res := authResult{}
	return socket.loginRun(cred.Source, &cmd, &res, func() error {
		if !res.Ok {
			return errors.New(res.ErrMsg)
		}
		socket.Lock()
		socket.dropAuth(cred.Source)
		socket.creds = append(socket.creds, cred)
		socket.Unlock()
		return nil
	})
}

type authX509Cmd struct {
	Authenticate int
	User         string
	Mechanism    string
}

func (socket *MongoSocket) loginX509(cred Credential) error {
	cmd := authX509Cmd{Authenticate: 1, User: cred.Username, Mechanism: "MONGODB-X509"}
	res := authResult{}
	return socket.loginRun(cred.Source, &cmd, &res, func() error {
		if !res.Ok {
			return errors.New(res.ErrMsg)
		}
		socket.Lock()
		socket.dropAuth(cred.Source)
		socket.creds = append(socket.creds, cred)
		socket.Unlock()
		return nil
	})
}

func (socket *MongoSocket) loginPlain(cred Credential) error {
	cmd := saslCmd{Start: 1, Mechanism: "PLAIN", Payload: []byte("\x00" + cred.Username + "\x00" + cred.Password)}
	res := authResult{}
	return socket.loginRun(cred.Source, &cmd, &res, func() error {
		if !res.Ok {
			return errors.New(res.ErrMsg)
		}
		socket.Lock()
		socket.dropAuth(cred.Source)
		socket.creds = append(socket.creds, cred)
		socket.Unlock()
		return nil
	})
}

func (socket *MongoSocket) loginSASL(cred Credential) error {
	var sasl saslStepper
	var err error
	if cred.Mechanism == "SCRAM-SHA-1" {
		// SCRAM is handled without external libraries.
		sasl = saslNewScram(cred)
	} else if len(cred.ServiceHost) > 0 {
		sasl, err = saslNew(cred, cred.ServiceHost)
	} else {
		sasl, err = saslNew(cred, socket.Server().Addr)
	}
	if err != nil {
		return err
	}
	defer sasl.Close()

	// The goal of this logic is to carry a locked socket until the
	// local SASL step confirms the auth is valid; the socket needs to be
	// locked so that concurrent action doesn't leave the socket in an
	// auth state that doesn't reflect the operations that took place.
	// As a simple case, imagine inverting login=>logout to logout=>login.
	//
	// The logic below works because the lock func isn't called concurrently.
	locked := false
	lock := func(b bool) {
		if locked != b {
			locked = b
			if b {
				socket.Lock()
			} else {
				socket.Unlock()
			}
		}
	}

	lock(true)
	defer lock(false)

	start := 1
	cmd := saslCmd{}
	res := saslResult{}
	for {
		payload, done, err := sasl.Step(res.Payload)
		if err != nil {
			return err
		}
		if done && res.Done {
			socket.dropAuth(cred.Source)
			socket.creds = append(socket.creds, cred)
			break
		}
		lock(false)

		cmd = saslCmd{
			Start:          start,
			Continue:       1 - start,
			ConversationId: res.ConversationId,
			Mechanism:      cred.Mechanism,
			Payload:        payload,
		}
		start = 0
		err = socket.loginRun(cred.Source, &cmd, &res, func() error {
			// See the comment on lock for why this is necessary.
			lock(true)
			if !res.Ok || res.NotOk {
				return fmt.Errorf("server returned error on SASL authentication step: %s", res.ErrMsg)
			}
			return nil
		})
		if err != nil {
			return err
		}
		if done && res.Done {
			socket.dropAuth(cred.Source)
			socket.creds = append(socket.creds, cred)
			break
		}
	}

	return nil
}

func saslNewScram(cred Credential) *saslScram {
	credsum := md5.New()
	credsum.Write([]byte(cred.Username + ":mongo:" + cred.Password))
	client := scram.NewClient(sha1.New, cred.Username, hex.EncodeToString(credsum.Sum(nil)))
	return &saslScram{cred: cred, client: client}
}

type saslScram struct {
	cred   Credential
	client *scram.Client
}

func (s *saslScram) Close() {}

func (s *saslScram) Step(serverData []byte) (clientData []byte, done bool, err error) {
	more := s.client.Step(serverData)
	return s.client.Out(), !more, s.client.Err()
}

func (socket *MongoSocket) loginRun(db string, query, result interface{}, f func() error) error {
	var mutex sync.Mutex
	var replyErr error
	mutex.Lock()

	op := QueryOp{}
	op.Query = query
	op.Collection = db + ".$cmd"
	op.Limit = -1
	op.replyFunc = func(err error, rfl *replyFuncLegacyArgs, rfc *replyFuncCommandArgs, rfm *replyFuncMsgArgs) {
		defer mutex.Unlock()

		if err != nil {
			replyErr = err
			return
		}

		err = bson.Unmarshal(rfl.docData, result)
		if err != nil {
			replyErr = err
		} else {
			// Must handle this within the read loop for the socket, so
			// that concurrent login requests are properly ordered.
			replyErr = f()
		}
	}

	err := socket.Query(&op)
	if err != nil {
		return err
	}
	mutex.Lock() // Wait.
	return replyErr
}

func (socket *MongoSocket) Logout(db string) {
	socket.Lock()
	cred, found := socket.dropAuth(db)
	if found {
		debugf("Socket %p to %s: logout: db=%q (flagged)", socket, socket.addr, db)
		socket.logout = append(socket.logout, cred)
	}
	socket.Unlock()
}

func (socket *MongoSocket) LogoutAll() {
	socket.Lock()
	if l := len(socket.creds); l > 0 {
		debugf("Socket %p to %s: logout all (flagged %d)", socket, socket.addr, l)
		socket.logout = append(socket.logout, socket.creds...)
		socket.creds = socket.creds[0:0]
	}
	socket.Unlock()
}

func (socket *MongoSocket) flushLogout() (ops []interface{}) {
	socket.Lock()
	if l := len(socket.logout); l > 0 {
		debugf("Socket %p to %s: logout all (flushing %d)", socket, socket.addr, l)
		for i := 0; i != l; i++ {
			op := QueryOp{}
			op.Query = &logoutCmd{1}
			op.Collection = socket.logout[i].Source + ".$cmd"
			op.Limit = -1
			ops = append(ops, &op)
		}
		socket.logout = socket.logout[0:0]
	}
	socket.Unlock()
	return
}

func (socket *MongoSocket) dropAuth(db string) (cred Credential, found bool) {
	for i, sockCred := range socket.creds {
		if sockCred.Source == db {
			copy(socket.creds[i:], socket.creds[i+1:])
			socket.creds = socket.creds[:len(socket.creds)-1]
			return sockCred, true
		}
	}
	return cred, false
}

func (socket *MongoSocket) dropLogout(cred Credential) (found bool) {
	for i, sockCred := range socket.logout {
		if sockCred == cred {
			copy(socket.logout[i:], socket.logout[i+1:])
			socket.logout = socket.logout[:len(socket.logout)-1]
			return true
		}
	}
	return false
}
