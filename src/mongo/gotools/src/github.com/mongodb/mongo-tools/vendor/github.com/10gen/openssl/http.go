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

import (
	"net/http"
)

// ListenAndServeTLS will take an http.Handler and serve it using OpenSSL over
// the given tcp address, configured to use the provided cert and key files.
func ListenAndServeTLS(addr string, cert_file string, key_file string,
	handler http.Handler) error {
	return ServerListenAndServeTLS(
		&http.Server{Addr: addr, Handler: handler}, cert_file, key_file)
}

// ServerListenAndServeTLS will take an http.Server and serve it using OpenSSL
// configured to use the provided cert and key files.
func ServerListenAndServeTLS(srv *http.Server,
	cert_file, key_file string) error {
	addr := srv.Addr
	if addr == "" {
		addr = ":https"
	}

	ctx, err := NewCtxFromFiles(cert_file, key_file)
	if err != nil {
		return err
	}

	l, err := Listen("tcp", addr, ctx)
	if err != nil {
		return err
	}

	return srv.Serve(l)
}

// TODO: http client integration
// holy crap, getting this integrated nicely with the Go stdlib HTTP client
// stack so that it does proxying, connection pooling, and most importantly
// hostname verification is really hard. So much stuff is hardcoded to just use
// the built-in TLS lib. I think to get this to work either some crazy
// hacktackery beyond me, an almost straight up fork of the HTTP client, or
// serious stdlib internal refactoring is necessary.
// even more so, good luck getting openssl to use the operating system default
// root certificates if the user doesn't provide any. sadlol
// NOTE: if you're going to try and write your own round tripper, at least use
//  openssl.Dial, or equivalent logic
