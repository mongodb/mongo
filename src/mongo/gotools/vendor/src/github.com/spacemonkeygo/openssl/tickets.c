// Copyright (C) 2015 Space Monkey, Inc.
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
#include <openssl/evp.h>
#include "_cgo_export.h"

int ticket_key_cb(SSL *s, unsigned char key_name[16],
		unsigned char iv[EVP_MAX_IV_LENGTH],
		EVP_CIPHER_CTX *cctx, HMAC_CTX *hctx, int enc) {

	SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(s);
	void* p = SSL_CTX_get_ex_data(ssl_ctx, get_ssl_ctx_idx());
	// get the pointer to the go Ctx object and pass it back into the thunk
	return ticket_key_cb_thunk(p, s, key_name, iv, cctx, hctx, enc);
}
