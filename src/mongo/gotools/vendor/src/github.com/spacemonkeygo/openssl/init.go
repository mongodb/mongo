// Copyright (C) 2014 Space Monkey, Inc.
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

// +build cgo

/*
Package openssl is a light wrapper around OpenSSL for Go.

It strives to provide a near-drop-in replacement for the Go standard library
tls package, while allowing for:

Performance

OpenSSL is battle-tested and optimized C. While Go's built-in library shows
great promise, it is still young and in some places, inefficient. This simple
OpenSSL wrapper can often do at least 2x with the same cipher and protocol.

On my lappytop, I get the following benchmarking speeds:
  BenchmarkSHA1Large_openssl      1000  2611282 ns/op  401.56 MB/s
  BenchmarkSHA1Large_stdlib        500  3963983 ns/op  264.53 MB/s
  BenchmarkSHA1Small_openssl   1000000     3476 ns/op    0.29 MB/s
  BenchmarkSHA1Small_stdlib    5000000      550 ns/op    1.82 MB/s
  BenchmarkSHA256Large_openssl     200  8085314 ns/op  129.69 MB/s
  BenchmarkSHA256Large_stdlib      100 18948189 ns/op   55.34 MB/s
  BenchmarkSHA256Small_openssl 1000000     4262 ns/op    0.23 MB/s
  BenchmarkSHA256Small_stdlib  1000000     1444 ns/op    0.69 MB/s
  BenchmarkOpenSSLThroughput    100000    21634 ns/op   47.33 MB/s
  BenchmarkStdlibThroughput      50000    58974 ns/op   17.36 MB/s

Interoperability

Many systems support OpenSSL with a variety of plugins and modules for things,
such as hardware acceleration in embedded devices.

Greater flexibility and configuration

OpenSSL allows for far greater configuration of corner cases and backwards
compatibility (such as support of SSLv2). You shouldn't be using SSLv2 if you
can help but, but sometimes you can't help it.

Security

Yeah yeah, Heartbleed. But according to the author of the standard library's
TLS implementation, Go's TLS library is vulnerable to timing attacks. And
whether or not OpenSSL received the appropriate amount of scrutiny
pre-Heartbleed, it sure is receiving it now.

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

Help wanted: To get this library to work with net/http's client, we
had to fork net/http. It would be nice if an alternate http client library
supported the generality needed to use OpenSSL instead of crypto/tls.
*/
package openssl

/*
#include <openssl/ssl.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/engine.h>

extern int Goopenssl_init_locks();
extern void Goopenssl_thread_locking_callback(int, int, const char*, int);

static int Goopenssl_init_threadsafety() {
	// Set up OPENSSL thread safety callbacks.  We only set the locking
	// callback because the default id callback implementation is good
	// enough for us.
	int rc = Goopenssl_init_locks();
	if (rc == 0) {
		CRYPTO_set_locking_callback(Goopenssl_thread_locking_callback);
	}
	return rc;
}

static void OpenSSL_add_all_algorithms_not_a_macro() {
	OpenSSL_add_all_algorithms();
}

*/
import "C"

import (
	"errors"
	"fmt"
	"strings"
)

func init() {
	C.ERR_load_crypto_strings()
	C.OPENSSL_config(nil)
	C.ENGINE_load_builtin_engines()
	C.SSL_load_error_strings()
	C.SSL_library_init()
	C.OpenSSL_add_all_algorithms_not_a_macro()
	rc := C.Goopenssl_init_threadsafety()
	if rc != 0 {
		panic(fmt.Errorf("Goopenssl_init_locks failed with %d", rc))
	}
}

// errorFromErrorQueue needs to run in the same OS thread as the operation
// that caused the possible error
func errorFromErrorQueue() error {
	var errs []string
	for {
		err := C.ERR_get_error()
		if err == 0 {
			break
		}
		errs = append(errs, fmt.Sprintf("%s:%s:%s",
			C.GoString(C.ERR_lib_error_string(err)),
			C.GoString(C.ERR_func_error_string(err)),
			C.GoString(C.ERR_reason_error_string(err))))
	}
	return errors.New(fmt.Sprintf("SSL errors: %s", strings.Join(errs, "\n")))
}
