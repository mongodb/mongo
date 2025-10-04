/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2019-2022, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


/**
 * @name SSL certificates
 *
 */

#include "rdkafka_int.h"
#include "rdkafka_transport_int.h"


#if WITH_SSL
#include "rdkafka_ssl.h"

#include <openssl/x509.h>
#include <openssl/evp.h>

/**
 * @brief OpenSSL password query callback using a conf struct.
 *
 * @locality application thread
 */
static int
rd_kafka_conf_ssl_passwd_cb(char *buf, int size, int rwflag, void *userdata) {
        const rd_kafka_conf_t *conf = userdata;
        int pwlen;

        if (!conf->ssl.key_password)
                return -1;

        pwlen = (int)strlen(conf->ssl.key_password);
        memcpy(buf, conf->ssl.key_password, RD_MIN(pwlen, size));

        return pwlen;
}



static const char *rd_kafka_cert_type_names[] = {"public-key", "private-key",
                                                 "CA"};

static const char *rd_kafka_cert_enc_names[] = {"PKCS#12", "DER", "PEM"};


/**
 * @brief Destroy a certificate
 */
static void rd_kafka_cert_destroy(rd_kafka_cert_t *cert) {
        if (rd_refcnt_sub(&cert->refcnt) > 0)
                return;

        if (cert->x509)
                X509_free(cert->x509);
        if (cert->pkey)
                EVP_PKEY_free(cert->pkey);
        if (cert->store)
                X509_STORE_free(cert->store);

        rd_free(cert);
}


/**
 * @brief Create a copy of a cert
 */
static rd_kafka_cert_t *rd_kafka_cert_dup(rd_kafka_cert_t *src) {
        rd_refcnt_add(&src->refcnt);
        return src;
}


#if OPENSSL_VERSION_NUMBER < 0x30000000
/**
 * @brief Print the OpenSSL error stack to stdout, for development use.
 */
static RD_UNUSED void rd_kafka_print_ssl_errors(void) {
        unsigned long l;
        const char *file, *data;
        int line, flags;

        while ((l = ERR_get_error_line_data(&file, &line, &data, &flags)) !=
               0) {
                char buf[256];

                ERR_error_string_n(l, buf, sizeof(buf));

                printf("ERR: %s:%d: %s: %s:\n", file, line, buf,
                       (flags & ERR_TXT_STRING) ? data : "");
                printf("  %lu:%s : %s : %s : %d : %s (%p, %d, fl 0x%x)\n", l,
                       ERR_lib_error_string(l), ERR_func_error_string(l), file,
                       line,
                       (flags & ERR_TXT_STRING) && data && *data
                           ? data
                           : ERR_reason_error_string(l),
                       data, data ? (int)strlen(data) : -1,
                       flags & ERR_TXT_STRING);
        }
}
#endif


/**
 * @returns a cert structure with a copy of the memory in \p buffer on success,
 *          or NULL on failure in which case errstr will have a human-readable
 *          error string written to it.
 */
static rd_kafka_cert_t *rd_kafka_cert_new(const rd_kafka_conf_t *conf,
                                          rd_kafka_cert_type_t type,
                                          rd_kafka_cert_enc_t encoding,
                                          const void *buffer,
                                          size_t size,
                                          char *errstr,
                                          size_t errstr_size) {
        static const rd_bool_t
            valid[RD_KAFKA_CERT__CNT][RD_KAFKA_CERT_ENC__CNT] = {
                /* Valid encodings per certificate type */
                [RD_KAFKA_CERT_PUBLIC_KEY] = {[RD_KAFKA_CERT_ENC_PKCS12] =
                                                  rd_true,
                                              [RD_KAFKA_CERT_ENC_DER] = rd_true,
                                              [RD_KAFKA_CERT_ENC_PEM] =
                                                  rd_true},
                [RD_KAFKA_CERT_PRIVATE_KEY] =
                    {[RD_KAFKA_CERT_ENC_PKCS12] = rd_true,
                     [RD_KAFKA_CERT_ENC_DER]    = rd_true,
                     [RD_KAFKA_CERT_ENC_PEM]    = rd_true},
                [RD_KAFKA_CERT_CA] = {[RD_KAFKA_CERT_ENC_PKCS12] = rd_true,
                                      [RD_KAFKA_CERT_ENC_DER]    = rd_true,
                                      [RD_KAFKA_CERT_ENC_PEM]    = rd_true},
            };
        const char *action = "", *ssl_errstr = NULL, *extra = "";
        BIO *bio;
        rd_kafka_cert_t *cert = NULL;
        PKCS12 *p12           = NULL;

        if ((int)type < 0 || type >= RD_KAFKA_CERT__CNT) {
                rd_snprintf(errstr, errstr_size, "Invalid certificate type %d",
                            (int)type);
                return NULL;
        }

        if ((int)encoding < 0 || encoding >= RD_KAFKA_CERT_ENC__CNT) {
                rd_snprintf(errstr, errstr_size,
                            "Invalid certificate encoding %d", (int)encoding);
                return NULL;
        }

        if (!valid[type][encoding]) {
                rd_snprintf(errstr, errstr_size,
                            "Invalid encoding %s for certificate type %s",
                            rd_kafka_cert_enc_names[encoding],
                            rd_kafka_cert_type_names[type]);
                return NULL;
        }

        action = "read memory";
        bio    = BIO_new_mem_buf((void *)buffer, (long)size);
        if (!bio)
                goto fail;

        if (encoding == RD_KAFKA_CERT_ENC_PKCS12) {
                action = "read PKCS#12";
                p12    = d2i_PKCS12_bio(bio, NULL);
                if (!p12)
                        goto fail;
        }

        cert           = rd_calloc(1, sizeof(*cert));
        cert->type     = type;
        cert->encoding = encoding;

        rd_refcnt_init(&cert->refcnt, 1);

        switch (type) {
        case RD_KAFKA_CERT_CA:
                cert->store = X509_STORE_new();

                switch (encoding) {
                case RD_KAFKA_CERT_ENC_PKCS12: {
                        EVP_PKEY *ign_pkey;
                        X509 *ign_cert;
                        STACK_OF(X509) *cas = NULL;
                        int i;

                        action = "parse PKCS#12";
                        if (!PKCS12_parse(p12, conf->ssl.key_password,
                                          &ign_pkey, &ign_cert, &cas))
                                goto fail;

                        EVP_PKEY_free(ign_pkey);
                        X509_free(ign_cert);

                        if (!cas || sk_X509_num(cas) < 1) {
                                action =
                                    "retrieve at least one CA "
                                    "cert from PKCS#12";
                                if (cas)
                                        sk_X509_pop_free(cas, X509_free);
                                goto fail;
                        }

                        for (i = 0; i < sk_X509_num(cas); i++) {
                                if (!X509_STORE_add_cert(
                                        cert->store, sk_X509_value(cas, i))) {
                                        action =
                                            "add certificate to "
                                            "X.509 store";
                                        sk_X509_pop_free(cas, X509_free);
                                        goto fail;
                                }
                        }

                        sk_X509_pop_free(cas, X509_free);
                } break;

                case RD_KAFKA_CERT_ENC_DER: {
                        X509 *x509;

                        action = "read DER / X.509 ASN.1";
                        if (!(x509 = d2i_X509_bio(bio, NULL)))
                                goto fail;

                        if (!X509_STORE_add_cert(cert->store, x509)) {
                                action =
                                    "add certificate to "
                                    "X.509 store";
                                X509_free(x509);
                                goto fail;
                        }

                        X509_free(x509);
                } break;

                case RD_KAFKA_CERT_ENC_PEM: {
                        X509 *x509;
                        int cnt = 0;

                        action = "read PEM";

                        /* This will read one certificate per call
                         * until an error occurs or the end of the
                         * buffer is reached (which is an error
                         * we'll need to clear). */
                        while ((x509 = PEM_read_bio_X509(
                                    bio, NULL, rd_kafka_conf_ssl_passwd_cb,
                                    (void *)conf))) {

                                if (!X509_STORE_add_cert(cert->store, x509)) {
                                        action =
                                            "add certificate to "
                                            "X.509 store";
                                        X509_free(x509);
                                        goto fail;
                                }

                                X509_free(x509);
                                cnt++;
                        }

                        if (!BIO_eof(bio)) {
                                /* Encountered parse error before
                                 * reaching end, propagate error and
                                 * fail. */
                                goto fail;
                        }

                        if (!cnt) {
                                action =
                                    "retrieve at least one "
                                    "CA cert from PEM";

                                goto fail;
                        }

                        /* Reached end, which is raised as an error,
                         * so clear it since it is not. */
                        ERR_clear_error();
                } break;

                default:
                        RD_NOTREACHED();
                        break;
                }
                break;


        case RD_KAFKA_CERT_PUBLIC_KEY:
                switch (encoding) {
                case RD_KAFKA_CERT_ENC_PKCS12: {
                        EVP_PKEY *ign_pkey;

                        action = "parse PKCS#12";
                        if (!PKCS12_parse(p12, conf->ssl.key_password,
                                          &ign_pkey, &cert->x509, NULL))
                                goto fail;

                        EVP_PKEY_free(ign_pkey);

                        action = "retrieve public key";
                        if (!cert->x509)
                                goto fail;
                } break;

                case RD_KAFKA_CERT_ENC_DER:
                        action     = "read DER / X.509 ASN.1";
                        cert->x509 = d2i_X509_bio(bio, NULL);
                        if (!cert->x509)
                                goto fail;
                        break;

                case RD_KAFKA_CERT_ENC_PEM:
                        action     = "read PEM";
                        cert->x509 = PEM_read_bio_X509(
                            bio, NULL, rd_kafka_conf_ssl_passwd_cb,
                            (void *)conf);
                        if (!cert->x509)
                                goto fail;
                        break;

                default:
                        RD_NOTREACHED();
                        break;
                }
                break;


        case RD_KAFKA_CERT_PRIVATE_KEY:
                switch (encoding) {
                case RD_KAFKA_CERT_ENC_PKCS12: {
                        X509 *x509;

                        action = "parse PKCS#12";
                        if (!PKCS12_parse(p12, conf->ssl.key_password,
                                          &cert->pkey, &x509, NULL))
                                goto fail;

                        X509_free(x509);

                        action = "retrieve private key";
                        if (!cert->pkey)
                                goto fail;
                } break;

                case RD_KAFKA_CERT_ENC_DER:
                        action =
                            "read DER / X.509 ASN.1 and "
                            "convert to EVP_PKEY";
                        cert->pkey = d2i_PrivateKey_bio(bio, NULL);
                        if (!cert->pkey)
                                goto fail;
                        break;

                case RD_KAFKA_CERT_ENC_PEM:
                        action     = "read PEM";
                        cert->pkey = PEM_read_bio_PrivateKey(
                            bio, NULL, rd_kafka_conf_ssl_passwd_cb,
                            (void *)conf);
                        if (!cert->pkey)
                                goto fail;
                        break;

                default:
                        RD_NOTREACHED();
                        break;
                }
                break;

        default:
                RD_NOTREACHED();
                break;
        }

        if (bio)
                BIO_free(bio);
        if (p12)
                PKCS12_free(p12);

        return cert;

fail:
        ssl_errstr = rd_kafka_ssl_last_error_str();

        /* OpenSSL 3.x does not provide obsolete ciphers out of the box, so
         * let's try to identify such an error message and guide the user
         * to what to do (set up a provider config file and point to it
         * through the OPENSSL_CONF environment variable).
         * We could call OSSL_PROVIDER_load("legacy") here, but that would be
         * a non-obvious side-effect of calling this set function. */
        if (strstr(action, "parse") && strstr(ssl_errstr, "Algorithm"))
                extra =
                    ": legacy ciphers may require loading OpenSSL's \"legacy\" "
                    "provider through an OPENSSL_CONF configuration file";

        rd_snprintf(errstr, errstr_size, "Failed to %s %s (encoding %s): %s%s",
                    action, rd_kafka_cert_type_names[type],
                    rd_kafka_cert_enc_names[encoding], ssl_errstr, extra);

        if (cert)
                rd_kafka_cert_destroy(cert);
        if (bio)
                BIO_free(bio);
        if (p12)
                PKCS12_free(p12);

        return NULL;
}
#endif /* WITH_SSL */


/**
 * @name Public API
 * @brief These public methods must be available regardless if
 *        librdkafka was built with OpenSSL or not.
 * @{
 */

rd_kafka_conf_res_t rd_kafka_conf_set_ssl_cert(rd_kafka_conf_t *conf,
                                               rd_kafka_cert_type_t cert_type,
                                               rd_kafka_cert_enc_t cert_enc,
                                               const void *buffer,
                                               size_t size,
                                               char *errstr,
                                               size_t errstr_size) {
#if !WITH_SSL
        rd_snprintf(errstr, errstr_size,
                    "librdkafka not built with OpenSSL support");
        return RD_KAFKA_CONF_INVALID;
#else
        rd_kafka_cert_t *cert;
        rd_kafka_cert_t **cert_map[RD_KAFKA_CERT__CNT] = {
            [RD_KAFKA_CERT_PUBLIC_KEY]  = &conf->ssl.cert,
            [RD_KAFKA_CERT_PRIVATE_KEY] = &conf->ssl.key,
            [RD_KAFKA_CERT_CA]          = &conf->ssl.ca};
        rd_kafka_cert_t **certp;

        if ((int)cert_type < 0 || cert_type >= RD_KAFKA_CERT__CNT) {
                rd_snprintf(errstr, errstr_size, "Invalid certificate type %d",
                            (int)cert_type);
                return RD_KAFKA_CONF_INVALID;
        }

        /* Make sure OpenSSL is loaded */
        rd_kafka_global_init();

        certp = cert_map[cert_type];

        if (!buffer) {
                /* Clear current value */
                if (*certp) {
                        rd_kafka_cert_destroy(*certp);
                        *certp = NULL;
                }
                return RD_KAFKA_CONF_OK;
        }

        cert = rd_kafka_cert_new(conf, cert_type, cert_enc, buffer, size,
                                 errstr, errstr_size);
        if (!cert)
                return RD_KAFKA_CONF_INVALID;

        if (*certp)
                rd_kafka_cert_destroy(*certp);

        *certp = cert;

        return RD_KAFKA_CONF_OK;
#endif
}



/**
 * @brief Destructor called when configuration object is destroyed.
 */
void rd_kafka_conf_cert_dtor(int scope, void *pconf) {
#if WITH_SSL
        rd_kafka_conf_t *conf = pconf;
        assert(scope == _RK_GLOBAL);
        if (conf->ssl.key) {
                rd_kafka_cert_destroy(conf->ssl.key);
                conf->ssl.key = NULL;
        }
        if (conf->ssl.cert) {
                rd_kafka_cert_destroy(conf->ssl.cert);
                conf->ssl.cert = NULL;
        }
        if (conf->ssl.ca) {
                rd_kafka_cert_destroy(conf->ssl.ca);
                conf->ssl.ca = NULL;
        }
#endif
}

/**
 * @brief Copy-constructor called when configuration object \p psrcp is
 *        duplicated to \p dstp.
 */
void rd_kafka_conf_cert_copy(int scope,
                             void *pdst,
                             const void *psrc,
                             void *dstptr,
                             const void *srcptr,
                             size_t filter_cnt,
                             const char **filter) {
#if WITH_SSL
        rd_kafka_conf_t *dconf       = pdst;
        const rd_kafka_conf_t *sconf = psrc;

        assert(scope == _RK_GLOBAL);

        /* Free and reset any exist certs on the destination conf */
        rd_kafka_conf_cert_dtor(scope, pdst);

        if (sconf->ssl.key)
                dconf->ssl.key = rd_kafka_cert_dup(sconf->ssl.key);

        if (sconf->ssl.cert)
                dconf->ssl.cert = rd_kafka_cert_dup(sconf->ssl.cert);

        if (sconf->ssl.ca)
                dconf->ssl.ca = rd_kafka_cert_dup(sconf->ssl.ca);
#endif
}


/**@}*/
