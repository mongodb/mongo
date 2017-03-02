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

#include <openssl/ssl.h>
#include "_cgo_export.h"

int verify_cb(int ok, X509_STORE_CTX* store) {
	SSL* ssl = (SSL *)X509_STORE_CTX_get_app_data(store);
	SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
	void* p = SSL_CTX_get_ex_data(ssl_ctx, get_ssl_ctx_idx());
	// get the pointer to the go Ctx object and pass it back into the thunk
	return verify_cb_thunk(p, ok, store);
}

int verify_ssl_cb(int ok, X509_STORE_CTX* store) {
	SSL* ssl = (SSL *)X509_STORE_CTX_get_app_data(store);
	void* p = SSL_get_ex_data(ssl, get_ssl_idx());
	// get the pointer to the go Ctx object and pass it back into the thunk
	return verify_ssl_cb_thunk(p, ok, store);
}
