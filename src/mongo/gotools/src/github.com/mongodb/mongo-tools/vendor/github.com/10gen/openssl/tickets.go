// Copyright (C) 2017. See AUTHORS.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package openssl

// #include "shim.h"
import "C"

import (
	"os"
	"unsafe"
)

const (
	KeyNameSize = 16
)

// TicketCipherCtx describes the cipher that will be used by the ticket store
// for encrypting the tickets. Engine may be nil if no engine is desired.
type TicketCipherCtx struct {
	Cipher *Cipher
	Engine *Engine
}

// TicketDigestCtx describes the digest that will be used by the ticket store
// to authenticate the data. Engine may be nil if no engine is desired.
type TicketDigestCtx struct {
	Digest *Digest
	Engine *Engine
}

// TicketName is an identifier for the key material for a ticket.
type TicketName [KeyNameSize]byte

// TicketKey is the key material for a ticket. If this is lost, forward secrecy
// is lost as it allows decrypting TLS sessions retroactively.
type TicketKey struct {
	Name      TicketName
	CipherKey []byte
	HMACKey   []byte
	IV        []byte
}

// TicketKeyManager is a manager for TicketKeys. It allows one to control the
// lifetime of tickets, causing renewals and expirations for keys that are
// created. Calls to the manager are serialized.
type TicketKeyManager interface {
	// New should create a brand new TicketKey with a new name.
	New() *TicketKey

	// Current should return a key that is still valid.
	Current() *TicketKey

	// Lookup should return a key with the given name, or nil if no name
	// exists.
	Lookup(name TicketName) *TicketKey

	// Expired should return if the key with the given name is expired and
	// should not be used any more.
	Expired(name TicketName) bool

	// ShouldRenew should return if the key is still ok to use for the current
	// session, but we should send a new key for the client.
	ShouldRenew(name TicketName) bool
}

// TicketStore descibes the encryption and authentication methods the tickets
// will use along with a key manager for generating and keeping track of the
// secrets.
type TicketStore struct {
	CipherCtx TicketCipherCtx
	DigestCtx TicketDigestCtx
	Keys      TicketKeyManager
}

func (t *TicketStore) cipherEngine() *C.ENGINE {
	if t.CipherCtx.Engine == nil {
		return nil
	}
	return t.CipherCtx.Engine.e
}

func (t *TicketStore) digestEngine() *C.ENGINE {
	if t.DigestCtx.Engine == nil {
		return nil
	}
	return t.DigestCtx.Engine.e
}

const (
	// instruct to do a handshake
	ticket_resp_requireHandshake = 0
	// crypto context is set up correctly
	ticket_resp_sessionOk = 1
	// crypto context is ok, but the ticket should be reissued
	ticket_resp_renewSession = 2
	// we had a problem that shouldn't fall back to doing a handshake
	ticket_resp_error = -1

	// asked to create session crypto context
	ticket_req_newSession = 1
	// asked to load crypto context for a previous session
	ticket_req_lookupSession = 0
)

//export go_ticket_key_cb_thunk
func go_ticket_key_cb_thunk(p unsafe.Pointer, s *C.SSL, key_name *C.uchar,
	iv *C.uchar, cctx *C.EVP_CIPHER_CTX, hctx *C.HMAC_CTX, enc C.int) C.int {

	// no panic's allowed. it's super hard to guarantee any state at this point
	// so just abort everything.
	defer func() {
		if err := recover(); err != nil {
			logger.Critf("openssl: ticket key callback panic'd: %v", err)
			os.Exit(1)
		}
	}()

	ctx := (*Ctx)(p)
	store := ctx.ticket_store
	if store == nil {
		// TODO(jeff): should this be an error condition? it doesn't make sense
		// to be called if we don't have a store I believe, but that's probably
		// not worth aborting the handshake which is what I believe returning
		// an error would do.
		return ticket_resp_requireHandshake
	}

	ctx.ticket_store_mu.Lock()
	defer ctx.ticket_store_mu.Unlock()

	switch enc {
	case ticket_req_newSession:
		key := store.Keys.Current()
		if key == nil {
			key = store.Keys.New()
			if key == nil {
				return ticket_resp_requireHandshake
			}
		}

		C.memcpy(
			unsafe.Pointer(key_name),
			unsafe.Pointer(&key.Name[0]),
			KeyNameSize)
		C.EVP_EncryptInit_ex(
			cctx,
			store.CipherCtx.Cipher.ptr,
			store.cipherEngine(),
			(*C.uchar)(&key.CipherKey[0]),
			(*C.uchar)(&key.IV[0]))
		C.HMAC_Init_ex(
			hctx,
			unsafe.Pointer(&key.HMACKey[0]),
			C.int(len(key.HMACKey)),
			store.DigestCtx.Digest.ptr,
			store.digestEngine())

		return ticket_resp_sessionOk

	case ticket_req_lookupSession:
		var name TicketName
		C.memcpy(
			unsafe.Pointer(&name[0]),
			unsafe.Pointer(key_name),
			KeyNameSize)

		key := store.Keys.Lookup(name)
		if key == nil {
			return ticket_resp_requireHandshake
		}
		if store.Keys.Expired(name) {
			return ticket_resp_requireHandshake
		}

		C.EVP_DecryptInit_ex(
			cctx,
			store.CipherCtx.Cipher.ptr,
			store.cipherEngine(),
			(*C.uchar)(&key.CipherKey[0]),
			(*C.uchar)(&key.IV[0]))
		C.HMAC_Init_ex(
			hctx,
			unsafe.Pointer(&key.HMACKey[0]),
			C.int(len(key.HMACKey)),
			store.DigestCtx.Digest.ptr,
			store.digestEngine())

		if store.Keys.ShouldRenew(name) {
			return ticket_resp_renewSession
		}

		return ticket_resp_sessionOk

	default:
		return ticket_resp_error
	}
}

// SetTicketStore sets the ticket store for the context so that clients can do
// ticket based session resumption. If the store is nil, the
func (c *Ctx) SetTicketStore(store *TicketStore) {
	c.ticket_store = store

	if store == nil {
		C.X_SSL_CTX_set_tlsext_ticket_key_cb(c.ctx, nil)
	} else {
		C.X_SSL_CTX_set_tlsext_ticket_key_cb(c.ctx,
			(*[0]byte)(C.X_SSL_CTX_ticket_key_cb))
	}
}
