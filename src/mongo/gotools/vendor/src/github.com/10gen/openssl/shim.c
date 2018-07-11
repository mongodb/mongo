/*
 * Copyright (C) 2014 Space Monkey, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <string.h>

#include "shim.h"

#include "_cgo_export.h"

/*
 * Functions defined in other .c files
 */
extern int go_init_locks();
extern unsigned long go_thread_id_callback();
extern void go_thread_locking_callback(int, int, const char*, int);
static int go_write_bio_puts(BIO *b, const char *str) {
	return go_write_bio_write(b, (char*)str, (int)strlen(str));
}

/*
 * Functions to convey openssl feature defines at runtime
 */
int X_OPENSSL_NO_ECDH() {
#ifdef OPENSSL_NO_ECDH
	return 1;
#else
	return 0;
#endif
}

/*
 ************************************************
 * v1.1.X and later implementation
 ************************************************
 */
#if OPENSSL_VERSION_NUMBER >= 0x1010000fL

void X_BIO_set_data(BIO* bio, void* data) {
	BIO_set_data(bio, data);
}

void* X_BIO_get_data(BIO* bio) {
	return BIO_get_data(bio);
}

EVP_MD_CTX* X_EVP_MD_CTX_new() {
	return EVP_MD_CTX_new();
}

void X_EVP_MD_CTX_free(EVP_MD_CTX* ctx) {
	EVP_MD_CTX_free(ctx);
}

static int x_bio_create(BIO *b) {
	BIO_set_shutdown(b, 1);
	BIO_set_init(b, 1);
	BIO_set_data(b, NULL);
	BIO_clear_flags(b, ~0);
	return 1;
}

static int x_bio_free(BIO *b) {
	return 1;
}

static BIO_METHOD *writeBioMethod;
static BIO_METHOD *readBioMethod;

BIO_METHOD* BIO_s_readBio() { return readBioMethod; }
BIO_METHOD* BIO_s_writeBio() { return writeBioMethod; }

int x_bio_init_methods() {
	writeBioMethod = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "Go Write BIO");
	if (!writeBioMethod) {
		return 1;
	}
	if (1 != BIO_meth_set_write(writeBioMethod,
				(int (*)(BIO *, const char *, int))go_write_bio_write)) {
		return 2;
	}
	if (1 != BIO_meth_set_puts(writeBioMethod, go_write_bio_puts)) {
		return 3;
	}
	if (1 != BIO_meth_set_ctrl(writeBioMethod, go_write_bio_ctrl)) {
		return 4;
	}
	if (1 != BIO_meth_set_create(writeBioMethod, x_bio_create)) {
		return 5;
	}
	if (1 != BIO_meth_set_destroy(writeBioMethod, x_bio_free)) {
		return 6;
	}

	readBioMethod = BIO_meth_new(BIO_TYPE_SOURCE_SINK, "Go Read BIO");
	if (!readBioMethod) {
		return 7;
	}
	if (1 != BIO_meth_set_read(readBioMethod, go_read_bio_read)) {
		return 8;
	}
	if (1 != BIO_meth_set_ctrl(readBioMethod, go_read_bio_ctrl)) {
		return 9;
	}
	if (1 != BIO_meth_set_create(readBioMethod, x_bio_create)) {
		return 10;
	}
	if (1 != BIO_meth_set_destroy(readBioMethod, x_bio_free)) {
		return 11;
	}

	return 0;
}

const EVP_MD *X_EVP_dss() {
	return NULL;
}

const EVP_MD *X_EVP_dss1() {
	return NULL;
}

const EVP_MD *X_EVP_sha() {
	return NULL;
}

int X_EVP_CIPHER_CTX_encrypting(const EVP_CIPHER_CTX *ctx) {
	return EVP_CIPHER_CTX_encrypting(ctx);
}

int X_X509_add_ref(X509* x509) {
	return X509_up_ref(x509);
}

const ASN1_TIME *X_X509_get0_notBefore(const X509 *x) {
	return X509_get0_notBefore(x);
}

const ASN1_TIME *X_X509_get0_notAfter(const X509 *x) {
	return X509_get0_notAfter(x);
}

HMAC_CTX *X_HMAC_CTX_new(void) {
	return HMAC_CTX_new();
}

void X_HMAC_CTX_free(HMAC_CTX *ctx) {
	HMAC_CTX_free(ctx);
}

int X_PEM_write_bio_PrivateKey_traditional(BIO *bio, EVP_PKEY *key, const EVP_CIPHER *enc, unsigned char *kstr, int klen, pem_password_cb *cb, void *u) {
	return PEM_write_bio_PrivateKey_traditional(bio, key, enc, kstr, klen, cb, u);
}

#endif



/*
 ************************************************
 * v1.0.X implementation
 ************************************************
 */
#if OPENSSL_VERSION_NUMBER < 0x1010000fL

static int x_bio_create(BIO *b) {
	b->shutdown = 1;
	b->init = 1;
	b->num = -1;
	b->ptr = NULL;
	b->flags = 0;
	return 1;
}

static int x_bio_free(BIO *b) {
	return 1;
}

static BIO_METHOD writeBioMethod = {
	BIO_TYPE_SOURCE_SINK,
	"Go Write BIO",
	(int (*)(BIO *, const char *, int))go_write_bio_write,
	NULL,
	go_write_bio_puts,
	NULL,
	go_write_bio_ctrl,
	x_bio_create,
	x_bio_free,
	NULL};

static BIO_METHOD* BIO_s_writeBio() { return &writeBioMethod; }

static BIO_METHOD readBioMethod = {
	BIO_TYPE_SOURCE_SINK,
	"Go Read BIO",
	NULL,
	go_read_bio_read,
	NULL,
	NULL,
	go_read_bio_ctrl,
	x_bio_create,
	x_bio_free,
	NULL};

static BIO_METHOD* BIO_s_readBio() { return &readBioMethod; }

int x_bio_init_methods() {
	/* statically initialized above */
	return 0;
}

void X_BIO_set_data(BIO* bio, void* data) {
	bio->ptr = data;
}

void* X_BIO_get_data(BIO* bio) {
	return bio->ptr;
}

EVP_MD_CTX* X_EVP_MD_CTX_new() {
	return EVP_MD_CTX_create();
}

void X_EVP_MD_CTX_free(EVP_MD_CTX* ctx) {
	EVP_MD_CTX_destroy(ctx);
}

int X_X509_add_ref(X509* x509) {
	CRYPTO_add(&x509->references, 1, CRYPTO_LOCK_X509);
	return 1;
}

const ASN1_TIME *X_X509_get0_notBefore(const X509 *x) {
	return x->cert_info->validity->notBefore;
}

const ASN1_TIME *X_X509_get0_notAfter(const X509 *x) {
	return x->cert_info->validity->notAfter;
}

const EVP_MD *X_EVP_dss() {
	return EVP_dss();
}

const EVP_MD *X_EVP_dss1() {
	return EVP_dss1();
}

const EVP_MD *X_EVP_sha() {
	return EVP_sha();
}

int X_EVP_CIPHER_CTX_encrypting(const EVP_CIPHER_CTX *ctx) {
	return ctx->encrypt;
}

HMAC_CTX *X_HMAC_CTX_new(void) {
	/* v1.1.0 uses a OPENSSL_zalloc to allocate the memory which does not exist
	 * in previous versions. malloc+memset to get the same behavior */
	HMAC_CTX *ctx = (HMAC_CTX *)OPENSSL_malloc(sizeof(HMAC_CTX));
	if (ctx) {
		memset(ctx, 0, sizeof(HMAC_CTX));
		HMAC_CTX_init(ctx);
	}
	return ctx;
}

void X_HMAC_CTX_free(HMAC_CTX *ctx) {
	if (ctx) {
		HMAC_CTX_cleanup(ctx);
		OPENSSL_free(ctx);
	}
}

int X_PEM_write_bio_PrivateKey_traditional(BIO *bio, EVP_PKEY *key, const EVP_CIPHER *enc, unsigned char *kstr, int klen, pem_password_cb *cb, void *u) {
#if OPENSSL_VERSION_NUMBER >  0x10000000L
	/* PEM_write_bio_PrivateKey always tries to use the PKCS8 format if it
	 * is available, instead of using the "traditional" format as stated in the
	 * OpenSSL man page.
	 * i2d_PrivateKey should give us the correct DER encoding, so we'll just
	 * use PEM_ASN1_write_bio directly to write the DER encoding with the correct
	 * type header. */

	int ppkey_id, pkey_base_id, ppkey_flags;
	const char *pinfo, *ppem_str;
	char pem_type_str[80];

	// Lookup the ASN1 method information to get the pem type
	if (EVP_PKEY_asn1_get0_info(&ppkey_id, &pkey_base_id, &ppkey_flags, &pinfo, &ppem_str, key->ameth) != 1) {
		return 0;
	}
	// Set up the PEM type string
	if (BIO_snprintf(pem_type_str, 80, "%s PRIVATE KEY", ppem_str) <= 0) {
		// Failed to write out the pem type string, something is really wrong.
		return 0;
	}
	// Write out everything to the BIO
	return PEM_ASN1_write_bio((i2d_of_void *)i2d_PrivateKey,
		pem_type_str, bio, key, enc, kstr, klen, cb, u);
#else
   return -1;
#endif
}

#endif



/*
 ************************************************
 * common implementation
 ************************************************
 */

int X_shim_init() {
	int rc = 0;

	OPENSSL_config(NULL);
	ENGINE_load_builtin_engines();
	SSL_load_error_strings();
	SSL_library_init();
	OpenSSL_add_all_algorithms();

#if OPENSSL_VERSION_NUMBER < 0x1010000fL
	// Set up OPENSSL thread safety callbacks.
	rc = go_init_locks();
	if (rc != 0) {
		return rc;
	}
	CRYPTO_set_locking_callback(go_thread_locking_callback);
	CRYPTO_set_id_callback(go_thread_id_callback);
#endif
	rc = x_bio_init_methods();
	if (rc != 0) {
		return rc;
	}

	return 0;
}

void * X_OPENSSL_malloc(size_t size) {
	return OPENSSL_malloc(size);
}

void X_OPENSSL_free(void *ref) {
	OPENSSL_free(ref);
}

long X_SSL_set_options(SSL* ssl, long options) {
	return SSL_set_options(ssl, options);
}

long X_SSL_get_options(SSL* ssl) {
	return SSL_get_options(ssl);
}

long X_SSL_clear_options(SSL* ssl, long options) {
	return SSL_clear_options(ssl, options);
}

long X_SSL_set_tlsext_host_name(SSL *ssl, const char *name) {
   return SSL_set_tlsext_host_name(ssl, name);
}
const char * X_SSL_get_cipher_name(const SSL *ssl) {
   return SSL_get_cipher_name(ssl);
}
int X_SSL_session_reused(SSL *ssl) {
    return SSL_session_reused(ssl);
}

int X_SSL_new_index() {
	return SSL_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

int X_SSL_verify_cb(int ok, X509_STORE_CTX* store) {
	SSL* ssl = (SSL *)X509_STORE_CTX_get_ex_data(store,
			SSL_get_ex_data_X509_STORE_CTX_idx());
	void* p = SSL_get_ex_data(ssl, get_ssl_idx());
	// get the pointer to the go Ctx object and pass it back into the thunk
	return go_ssl_verify_cb_thunk(p, ok, store);
}

const SSL_METHOD *X_SSLv23_method() {
	return SSLv23_method();
}

const SSL_METHOD *X_SSLv3_method() {
#ifndef OPENSSL_NO_SSL3_METHOD
	return SSLv3_method();
#else
	return NULL;
#endif
}

const SSL_METHOD *X_TLSv1_method() {
	return TLSv1_method();
}

const SSL_METHOD *X_TLSv1_1_method() {
#if OPENSSL_VERSION_NUMBER > 0x10000000L
	return TLSv1_1_method();
#else
	return NULL;
#endif
}

const SSL_METHOD *X_TLSv1_2_method() {
#if OPENSSL_VERSION_NUMBER > 0x10000000L
	return TLSv1_2_method();
#else
	return NULL;
#endif
}

int X_SSL_CTX_new_index() {
	return SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
}

long X_SSL_CTX_set_options(SSL_CTX* ctx, long options) {
	return SSL_CTX_set_options(ctx, options);
}

long X_SSL_CTX_clear_options(SSL_CTX* ctx, long options) {
	return SSL_CTX_clear_options(ctx, options);
}

long X_SSL_CTX_get_options(SSL_CTX* ctx) {
	return SSL_CTX_get_options(ctx);
}

long X_SSL_CTX_set_mode(SSL_CTX* ctx, long modes) {
	return SSL_CTX_set_mode(ctx, modes);
}

long X_SSL_CTX_get_mode(SSL_CTX* ctx) {
	return SSL_CTX_get_mode(ctx);
}

long X_SSL_CTX_set_session_cache_mode(SSL_CTX* ctx, long modes) {
	return SSL_CTX_set_session_cache_mode(ctx, modes);
}

long X_SSL_CTX_sess_set_cache_size(SSL_CTX* ctx, long t) {
	return SSL_CTX_sess_set_cache_size(ctx, t);
}

long X_SSL_CTX_sess_get_cache_size(SSL_CTX* ctx) {
	return SSL_CTX_sess_get_cache_size(ctx);
}

long X_SSL_CTX_set_timeout(SSL_CTX* ctx, long t) {
	return SSL_CTX_set_timeout(ctx, t);
}

long X_SSL_CTX_get_timeout(SSL_CTX* ctx) {
	return SSL_CTX_get_timeout(ctx);
}

long X_SSL_CTX_add_extra_chain_cert(SSL_CTX* ctx, X509 *cert) {
	return SSL_CTX_add_extra_chain_cert(ctx, cert);
}

long X_SSL_CTX_set_tlsext_servername_callback(
		SSL_CTX* ctx, int (*cb)(SSL *con, int *ad, void *args)) {
	return SSL_CTX_set_tlsext_servername_callback(ctx, cb);
}

int X_SSL_CTX_verify_cb(int ok, X509_STORE_CTX* store) {
	SSL* ssl = (SSL *)X509_STORE_CTX_get_ex_data(store,
			SSL_get_ex_data_X509_STORE_CTX_idx());
	SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
	void* p = SSL_CTX_get_ex_data(ssl_ctx, get_ssl_ctx_idx());
	// get the pointer to the go Ctx object and pass it back into the thunk
	return go_ssl_ctx_verify_cb_thunk(p, ok, store);
}

long X_SSL_CTX_set_tmp_dh(SSL_CTX* ctx, DH *dh) {
    return SSL_CTX_set_tmp_dh(ctx, dh);
}

long X_PEM_read_DHparams(SSL_CTX* ctx, DH *dh) {
    return SSL_CTX_set_tmp_dh(ctx, dh);
}

int X_SSL_CTX_set_tlsext_ticket_key_cb(SSL_CTX *sslctx,
        int (*cb)(SSL *s, unsigned char key_name[16],
                  unsigned char iv[EVP_MAX_IV_LENGTH],
                  EVP_CIPHER_CTX *ctx, HMAC_CTX *hctx, int enc)) {
    return SSL_CTX_set_tlsext_ticket_key_cb(sslctx, cb);
}

int X_SSL_CTX_ticket_key_cb(SSL *s, unsigned char key_name[16],
		unsigned char iv[EVP_MAX_IV_LENGTH],
		EVP_CIPHER_CTX *cctx, HMAC_CTX *hctx, int enc) {

	SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(s);
	void* p = SSL_CTX_get_ex_data(ssl_ctx, get_ssl_ctx_idx());
	// get the pointer to the go Ctx object and pass it back into the thunk
	return go_ticket_key_cb_thunk(p, s, key_name, iv, cctx, hctx, enc);
}

int X_BIO_get_flags(BIO *b) {
	return BIO_get_flags(b);
}

void X_BIO_set_flags(BIO *b, int flags) {
	return BIO_set_flags(b, flags);
}

void X_BIO_clear_flags(BIO *b, int flags) {
	BIO_clear_flags(b, flags);
}

int X_BIO_read(BIO *b, void *buf, int len) {
	return BIO_read(b, buf, len);
}

int X_BIO_write(BIO *b, const void *buf, int len) {
	return BIO_write(b, buf, len);
}

BIO *X_BIO_new_write_bio() {
	return BIO_new(BIO_s_writeBio());
}

BIO *X_BIO_new_read_bio() {
	return BIO_new(BIO_s_readBio());
}

const EVP_MD *X_EVP_get_digestbyname(const char *name) {
	return EVP_get_digestbyname(name);
}

const EVP_MD *X_EVP_md_null() {
	return EVP_md_null();
}

const EVP_MD *X_EVP_md5() {
	return EVP_md5();
}

const EVP_MD *X_EVP_ripemd160() {
	return EVP_ripemd160();
}

const EVP_MD *X_EVP_sha224() {
	return EVP_sha224();
}

const EVP_MD *X_EVP_sha1() {
	return EVP_sha1();
}

const EVP_MD *X_EVP_sha256() {
	return EVP_sha256();
}

const EVP_MD *X_EVP_sha384() {
	return EVP_sha384();
}

const EVP_MD *X_EVP_sha512() {
	return EVP_sha512();
}

int X_EVP_MD_size(const EVP_MD *md) {
	return EVP_MD_size(md);
}

int X_EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, ENGINE *impl) {
	return EVP_DigestInit_ex(ctx, type, impl);
}

int X_EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt) {
	return EVP_DigestUpdate(ctx, d, cnt);
}

int X_EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s) {
	return EVP_DigestFinal_ex(ctx, md, s);
}

int X_EVP_SignInit(EVP_MD_CTX *ctx, const EVP_MD *type) {
	return EVP_SignInit(ctx, type);
}

int X_EVP_SignUpdate(EVP_MD_CTX *ctx, const void *d, unsigned int cnt) {
	return EVP_SignUpdate(ctx, d, cnt);
}

EVP_PKEY *X_EVP_PKEY_new(void) {
	return EVP_PKEY_new();
}

void X_EVP_PKEY_free(EVP_PKEY *pkey) {
	EVP_PKEY_free(pkey);
}

int X_EVP_PKEY_size(EVP_PKEY *pkey) {
	return EVP_PKEY_size(pkey);
}

struct rsa_st *X_EVP_PKEY_get1_RSA(EVP_PKEY *pkey) {
	return EVP_PKEY_get1_RSA(pkey);
}

int X_EVP_PKEY_set1_RSA(EVP_PKEY *pkey, struct rsa_st *key) {
	return EVP_PKEY_set1_RSA(pkey, key);
}

int X_EVP_PKEY_assign_charp(EVP_PKEY *pkey, int type, char *key) {
	return EVP_PKEY_assign(pkey, type, key);
}



int X_EVP_SignFinal(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s, EVP_PKEY *pkey) {
	return EVP_SignFinal(ctx, md, s, pkey);
}

int X_EVP_VerifyInit(EVP_MD_CTX *ctx, const EVP_MD *type) {
	return EVP_VerifyInit(ctx, type);
}

int X_EVP_VerifyUpdate(EVP_MD_CTX *ctx, const void *d,
		unsigned int cnt) {
	return EVP_VerifyUpdate(ctx, d, cnt);
}

int X_EVP_VerifyFinal(EVP_MD_CTX *ctx, const unsigned char *sigbuf, unsigned int siglen, EVP_PKEY *pkey) {
	return EVP_VerifyFinal(ctx, sigbuf, siglen, pkey);
}

int X_EVP_CIPHER_block_size(EVP_CIPHER *c) {
    return EVP_CIPHER_block_size(c);
}

int X_EVP_CIPHER_key_length(EVP_CIPHER *c) {
    return EVP_CIPHER_key_length(c);
}

int X_EVP_CIPHER_iv_length(EVP_CIPHER *c) {
    return EVP_CIPHER_iv_length(c);
}

int X_EVP_CIPHER_nid(EVP_CIPHER *c) {
    return EVP_CIPHER_nid(c);
}

int X_EVP_CIPHER_CTX_block_size(EVP_CIPHER_CTX *ctx) {
    return EVP_CIPHER_CTX_block_size(ctx);
}

int X_EVP_CIPHER_CTX_key_length(EVP_CIPHER_CTX *ctx) {
    return EVP_CIPHER_CTX_key_length(ctx);
}

int X_EVP_CIPHER_CTX_iv_length(EVP_CIPHER_CTX *ctx) {
    return EVP_CIPHER_CTX_iv_length(ctx);
}

const EVP_CIPHER *X_EVP_CIPHER_CTX_cipher(EVP_CIPHER_CTX *ctx) {
    return EVP_CIPHER_CTX_cipher(ctx);
}

#if OPENSSL_VERSION_NUMBER >  0x10000000L
#ifndef OPENSSL_NO_EC
int X_EVP_PKEY_CTX_set_ec_paramgen_curve_nid(EVP_PKEY_CTX *ctx, int nid) {
	return EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, nid);
}
#else
int X_EVP_PKEY_CTX_set_ec_paramgen_curve_nid(EVP_PKEY_CTX *ctx, int nid) {
	return -2; // not supported
}
#endif
#endif

// END HERE

size_t X_HMAC_size(const HMAC_CTX *e) {
#if OPENSSL_VERSION_NUMBER >  0x10000000L
	return HMAC_size(e);
#else
  return 0;
#endif
}

int X_HMAC_Init_ex(HMAC_CTX *ctx, const void *key, int len, const EVP_MD *md, ENGINE *impl) {
#if OPENSSL_VERSION_NUMBER >  0x10000000L
	return HMAC_Init_ex(ctx, key, len, md, impl);
#else
  return -1;
#endif
}

int X_HMAC_Update(HMAC_CTX *ctx, const unsigned char *data, size_t len) {
#if OPENSSL_VERSION_NUMBER >  0x10000000L
	return HMAC_Update(ctx, data, len);
#else
  return -1;
#endif
}

int X_HMAC_Final(HMAC_CTX *ctx, unsigned char *md, unsigned int *len) {
#if OPENSSL_VERSION_NUMBER >  0x10000000L
	return HMAC_Final(ctx, md, len);
#else
  return -1;
#endif
}

int X_sk_X509_num(STACK_OF(X509) *sk) {
	return sk_X509_num(sk);
}

X509 *X_sk_X509_value(STACK_OF(X509)* sk, int i) {
   return sk_X509_value(sk, i);
}

#ifdef OPENSSL_FIPS
int X_FIPS_mode(void) {
    return FIPS_mode();
}
int X_FIPS_mode_set(int r) {
    return FIPS_mode_set(r);
}
#else
int X_FIPS_mode(void) {
    return 0;
}
int X_FIPS_mode_set(int r) {
    return 0;
}
#endif
