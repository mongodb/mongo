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

/*
Package openssl is a light wrapper around OpenSSL for Go.

This version has been forked from https://github.com/spacemonkeygo/openssl
for greater back-compatibility to older openssl libraries.

Usage

Starting an HTTP server that uses OpenSSL is very easy. It's as simple as:
  log.Fatal(openssl.ListenAndServeTLS(
        ":8443", "my_server.crt", "my_server.key", myHandler))

Getting a net.Listener that uses OpenSSL is also easy:
  ctx, err := openssl.NewCtxFromFiles("my_server.crt", "my_server.key")
  if err != nil {
          log.Fatal(err)
  }
  l, err := openssl.Listen("tcp", ":7777", ctx)

Making a client connection is straightforward too:
  ctx, err := NewCtx()
  if err != nil {
          log.Fatal(err)
  }
  err = ctx.LoadVerifyLocations("/etc/ssl/certs/ca-certificates.crt", "")
  if err != nil {
          log.Fatal(err)
  }
  conn, err := openssl.Dial("tcp", "localhost:7777", ctx, 0)

*/
package openssl

// #include "shim.h"
import "C"

import (
	"fmt"
	"strings"
)

func init() {
	if rc := C.X_shim_init(); rc != 0 {
		panic(fmt.Errorf("X_shim_init failed with %d", rc))
	}
}

// errorFromErrorQueue needs to run in the same OS thread as the operation
// that caused the possible error.  In some circumstances, ERR_get_error
// returns 0 when it shouldn't so we provide a message in that case.
func errorFromErrorQueue() error {
	var errs []string
	for {
		err := C.ERR_get_error()
		if err == 0 {
			break
		}
		errs = append(errs, fmt.Sprintf("%x:%s:%s:%s",
			err,
			C.GoString(C.ERR_lib_error_string(err)),
			C.GoString(C.ERR_func_error_string(err)),
			C.GoString(C.ERR_reason_error_string(err))))
	}
	if len(errs) == 0 {
		errs = append(errs, "0:Error unavailable")
	}
	return fmt.Errorf("SSL errors: %s", strings.Join(errs, "\n"))
}
