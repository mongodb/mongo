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

#include <stdlib.h>
#include <string.h>

#include <openssl/opensslconf.h>

#include <openssl/bio.h>
#include <openssl/conf.h>
#include <openssl/crypto.h>
#include <openssl/dh.h>
#include <openssl/engine.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#ifndef SSL_MODE_RELEASE_BUFFERS
#define SSL_MODE_RELEASE_BUFFERS 0
#endif

#ifndef SSL_OP_NO_COMPRESSION
#define SSL_OP_NO_COMPRESSION 0
#endif

#ifndef SSL_OP_NO_TLSv1_1
#define SSL_OP_NO_TLSv1_1 0
#endif

#ifndef SSL_OP_NO_TLSv1_2
#define SSL_OP_NO_TLSv1_2 0
#endif

/* shim  methods */
extern int X_shim_init();

/* Feature detection methods */
extern int X_OPENSSL_NO_ECDH();

/* Library methods */
extern void X_OPENSSL_free(void *ref);
extern void *X_OPENSSL_malloc(size_t size);

/* SSL methods */
extern long X_SSL_set_options(SSL* ssl, long options);
extern long X_SSL_get_options(SSL* ssl);
extern long X_SSL_clear_options(SSL* ssl, long options);
extern long X_SSL_set_tlsext_host_name(SSL *ssl, const char *name);
extern const char * X_SSL_get_cipher_name(const SSL *ssl);
extern int X_SSL_session_reused(SSL *ssl);
extern int X_SSL_new_index();

extern const SSL_METHOD *X_SSLv23_method();
extern const SSL_METHOD *X_SSLv3_method();
extern const SSL_METHOD *X_TLSv1_method();
extern const SSL_METHOD *X_TLSv1_1_method();
extern const SSL_METHOD *X_TLSv1_2_method();

#if defined SSL_CTRL_SET_TLSEXT_HOSTNAME
extern int sni_cb(SSL *ssl_conn, int *ad, void *arg);
#endif
extern int X_SSL_verify_cb(int ok, X509_STORE_CTX* store);

/* SSL_CTX methods */
extern int X_SSL_CTX_new_index();
extern long X_SSL_CTX_set_options(SSL_CTX* ctx, long options);
extern long X_SSL_CTX_clear_options(SSL_CTX* ctx, long options);
extern long X_SSL_CTX_get_options(SSL_CTX* ctx);
extern long X_SSL_CTX_set_mode(SSL_CTX* ctx, long modes);
extern long X_SSL_CTX_get_mode(SSL_CTX* ctx);
extern long X_SSL_CTX_set_session_cache_mode(SSL_CTX* ctx, long modes);
extern long X_SSL_CTX_sess_set_cache_size(SSL_CTX* ctx, long t);
extern long X_SSL_CTX_sess_get_cache_size(SSL_CTX* ctx);
extern long X_SSL_CTX_set_timeout(SSL_CTX* ctx, long t);
extern long X_SSL_CTX_get_timeout(SSL_CTX* ctx);
extern long X_SSL_CTX_add_extra_chain_cert(SSL_CTX* ctx, X509 *cert);
extern long X_SSL_CTX_set_tlsext_servername_callback(SSL_CTX* ctx, int (*cb)(SSL *con, int *ad, void *args));
extern int X_SSL_CTX_verify_cb(int ok, X509_STORE_CTX* store);
extern long X_SSL_CTX_set_tmp_dh(SSL_CTX* ctx, DH *dh);
extern long X_PEM_read_DHparams(SSL_CTX* ctx, DH *dh);
extern int X_SSL_CTX_set_tlsext_ticket_key_cb(SSL_CTX *sslctx,
        int (*cb)(SSL *s, unsigned char key_name[16],
                  unsigned char iv[EVP_MAX_IV_LENGTH],
                  EVP_CIPHER_CTX *ctx, HMAC_CTX *hctx, int enc));
extern int X_SSL_CTX_ticket_key_cb(SSL *s, unsigned char key_name[16],
        unsigned char iv[EVP_MAX_IV_LENGTH],
        EVP_CIPHER_CTX *cctx, HMAC_CTX *hctx, int enc);

/* BIO methods */
extern int X_BIO_get_flags(BIO *b);
extern void X_BIO_set_flags(BIO *bio, int flags);
extern void X_BIO_clear_flags(BIO *bio, int flags);
extern void X_BIO_set_data(BIO *bio, void* data);
extern void *X_BIO_get_data(BIO *bio);
extern int X_BIO_read(BIO *b, void *buf, int len);
extern int X_BIO_write(BIO *b, const void *buf, int len);
extern BIO *X_BIO_new_write_bio();
extern BIO *X_BIO_new_read_bio();

/* EVP methods */
extern const EVP_MD *X_EVP_get_digestbyname(const char *name);
extern EVP_MD_CTX *X_EVP_MD_CTX_new();
extern void X_EVP_MD_CTX_free(EVP_MD_CTX *ctx);
extern const EVP_MD *X_EVP_md_null();
extern const EVP_MD *X_EVP_md5();
extern const EVP_MD *X_EVP_sha();
extern const EVP_MD *X_EVP_sha1();
extern const EVP_MD *X_EVP_dss();
extern const EVP_MD *X_EVP_dss1();
extern const EVP_MD *X_EVP_ripemd160();
extern const EVP_MD *X_EVP_sha224();
extern const EVP_MD *X_EVP_sha256();
extern const EVP_MD *X_EVP_sha384();
extern const EVP_MD *X_EVP_sha512();
extern int X_EVP_MD_size(const EVP_MD *md);
extern int X_EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, ENGINE *impl);
extern int X_EVP_DigestUpdate(EVP_MD_CTX *ctx, const void *d, size_t cnt);
extern int X_EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s);
extern int X_EVP_SignInit(EVP_MD_CTX *ctx, const EVP_MD *type);
extern int X_EVP_SignUpdate(EVP_MD_CTX *ctx, const void *d, unsigned int cnt);
extern EVP_PKEY *X_EVP_PKEY_new(void);
extern void X_EVP_PKEY_free(EVP_PKEY *pkey);
extern int X_EVP_PKEY_size(EVP_PKEY *pkey);
extern struct rsa_st *X_EVP_PKEY_get1_RSA(EVP_PKEY *pkey);
extern int X_EVP_PKEY_set1_RSA(EVP_PKEY *pkey, struct rsa_st *key);
extern int X_EVP_PKEY_assign_charp(EVP_PKEY *pkey, int type, char *key);
extern int X_EVP_SignFinal(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *s, EVP_PKEY *pkey);
extern int X_EVP_VerifyInit(EVP_MD_CTX *ctx, const EVP_MD *type);
extern int X_EVP_VerifyUpdate(EVP_MD_CTX *ctx, const void *d, unsigned int cnt);
extern int X_EVP_VerifyFinal(EVP_MD_CTX *ctx, const unsigned char *sigbuf, unsigned int siglen, EVP_PKEY *pkey);
extern int X_EVP_CIPHER_block_size(EVP_CIPHER *c);
extern int X_EVP_CIPHER_key_length(EVP_CIPHER *c);
extern int X_EVP_CIPHER_iv_length(EVP_CIPHER *c);
extern int X_EVP_CIPHER_nid(EVP_CIPHER *c);
extern int X_EVP_CIPHER_CTX_block_size(EVP_CIPHER_CTX *ctx);
extern int X_EVP_CIPHER_CTX_key_length(EVP_CIPHER_CTX *ctx);
extern int X_EVP_CIPHER_CTX_iv_length(EVP_CIPHER_CTX *ctx);
extern const EVP_CIPHER *X_EVP_CIPHER_CTX_cipher(EVP_CIPHER_CTX *ctx);
extern int X_EVP_CIPHER_CTX_encrypting(const EVP_CIPHER_CTX *ctx);
#if OPENSSL_VERSION_NUMBER >  0x10000000L
extern int X_EVP_PKEY_CTX_set_ec_paramgen_curve_nid(EVP_PKEY_CTX *ctx, int nid);
#endif

/* HMAC methods */
extern size_t X_HMAC_size(const HMAC_CTX *e);
extern HMAC_CTX *X_HMAC_CTX_new(void);
extern void X_HMAC_CTX_free(HMAC_CTX *ctx);
extern int X_HMAC_Init_ex(HMAC_CTX *ctx, const void *key, int len, const EVP_MD *md, ENGINE *impl);
extern int X_HMAC_Update(HMAC_CTX *ctx, const unsigned char *data, size_t len);
extern int X_HMAC_Final(HMAC_CTX *ctx, unsigned char *md, unsigned int *len);

/* X509 methods */
extern int X_X509_add_ref(X509* x509);
extern const ASN1_TIME *X_X509_get0_notBefore(const X509 *x);
extern const ASN1_TIME *X_X509_get0_notAfter(const X509 *x);
extern int X_sk_X509_num(STACK_OF(X509) *sk);
extern X509 *X_sk_X509_value(STACK_OF(X509)* sk, int i);

/* PEM methods */
extern int X_PEM_write_bio_PrivateKey_traditional(BIO *bio, EVP_PKEY *key, const EVP_CIPHER *enc, unsigned char *kstr, int klen, pem_password_cb *cb, void *u);

/* FIPS methods */
extern int X_FIPS_mode(void);
extern int X_FIPS_mode_set(int r);
