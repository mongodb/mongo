/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2019-2022, Magnus Edenhill
 *               2023, Confluent Inc.
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
 * @name OpenSSL integration
 *
 */

#include "rdkafka_int.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_cert.h"

#ifdef _WIN32
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "libcrypto.lib")
#pragma comment(lib, "libssl.lib")
#endif

#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#if OPENSSL_VERSION_NUMBER >= 0x30000000
#include <openssl/provider.h>
#endif

#include <ctype.h>

#if !_WIN32
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


#if WITH_VALGRIND
/* OpenSSL relies on uninitialized memory, which Valgrind will whine about.
 * We use in-code Valgrind macros to suppress those warnings. */
#include <valgrind/memcheck.h>
#else
#define VALGRIND_MAKE_MEM_DEFINED(A, B)
#endif


#if OPENSSL_VERSION_NUMBER < 0x10100000L
static mtx_t *rd_kafka_ssl_locks;
static int rd_kafka_ssl_locks_cnt;
#endif


/**
 * @brief Close and destroy SSL session
 */
void rd_kafka_transport_ssl_close(rd_kafka_transport_t *rktrans) {
        SSL_shutdown(rktrans->rktrans_ssl);
        SSL_free(rktrans->rktrans_ssl);
        rktrans->rktrans_ssl = NULL;
}


/**
 * @brief Clear OpenSSL error queue to get a proper error reporting in case
 *        the next SSL_*() operation fails.
 */
static RD_INLINE void
rd_kafka_transport_ssl_clear_error(rd_kafka_transport_t *rktrans) {
        ERR_clear_error();
#ifdef _WIN32
        WSASetLastError(0);
#else
        rd_set_errno(0);
#endif
}

/**
 * @returns a thread-local single-invocation-use error string for
 *          the last thread-local error in OpenSSL, or an empty string
 *          if no error.
 */
const char *rd_kafka_ssl_last_error_str(void) {
        static RD_TLS char errstr[256];
        unsigned long l;
        const char *file, *data, *func;
        int line, flags;

#if OPENSSL_VERSION_NUMBER >= 0x30000000
        l = ERR_peek_last_error_all(&file, &line, &func, &data, &flags);
#else
        l    = ERR_peek_last_error_line_data(&file, &line, &data, &flags);
        func = ERR_func_error_string(l);
#endif

        if (!l)
                return "";

        rd_snprintf(errstr, sizeof(errstr), "%lu:%s:%s:%s:%d: %s", l,
                    ERR_lib_error_string(l), func, file, line,
                    ((flags & ERR_TXT_STRING) && data && *data)
                        ? data
                        : ERR_reason_error_string(l));

        return errstr;
}

/**
 * Serves the entire OpenSSL error queue and logs each error.
 * The last error is not logged but returned in 'errstr'.
 *
 * If 'rkb' is non-NULL broker-specific logging will be used,
 * else it will fall back on global 'rk' debugging.
 *
 * `ctx_identifier` is a string used to customize the log message.
 */
char *rd_kafka_ssl_error0(rd_kafka_t *rk,
                          rd_kafka_broker_t *rkb,
                          const char *ctx_identifier,
                          char *errstr,
                          size_t errstr_size) {
        unsigned long l;
        const char *file, *data, *func;
        int line, flags;
        int cnt = 0;

        if (!rk) {
                rd_assert(rkb);
                rk = rkb->rkb_rk;
        }

        while (
#if OPENSSL_VERSION_NUMBER >= 0x30000000
            (l = ERR_get_error_all(&file, &line, &func, &data, &flags))
#else
            (l = ERR_get_error_line_data(&file, &line, &data, &flags))
#endif
        ) {
                char buf[256];

#if OPENSSL_VERSION_NUMBER < 0x30000000
                func = ERR_func_error_string(l);
#endif

                if (cnt++ > 0) {
                        /* Log last message */
                        if (rkb)
                                rd_rkb_log(rkb, LOG_ERR, "SSL", "%s: %s",
                                           ctx_identifier, errstr);
                        else
                                rd_kafka_log(rk, LOG_ERR, "SSL", "%s: %s",
                                             ctx_identifier, errstr);
                }

                ERR_error_string_n(l, buf, sizeof(buf));

                if (!(flags & ERR_TXT_STRING) || !data || !*data)
                        data = NULL;

                /* Include openssl file:line:func if debugging is enabled */
                if (rk->rk_conf.log_level >= LOG_DEBUG)
                        rd_snprintf(errstr, errstr_size, "%s:%d:%s %s%s%s",
                                    file, line, func, buf, data ? ": " : "",
                                    data ? data : "");
                else
                        rd_snprintf(errstr, errstr_size, "%s%s%s", buf,
                                    data ? ": " : "", data ? data : "");
        }

        if (cnt == 0)
                rd_snprintf(errstr, errstr_size,
                            "%s: No further error information available",
                            ctx_identifier);

        return errstr;
}

static char *rd_kafka_ssl_error(rd_kafka_t *rk,
                                rd_kafka_broker_t *rkb,
                                char *errstr,
                                size_t errstr_size) {
        return rd_kafka_ssl_error0(rk, rkb, "kafka", errstr, errstr_size);
}

/**
 * Set transport IO event polling based on SSL error.
 *
 * Returns -1 on permanent errors.
 *
 * Locality: broker thread
 */
static RD_INLINE int
rd_kafka_transport_ssl_io_update(rd_kafka_transport_t *rktrans,
                                 int ret,
                                 char *errstr,
                                 size_t errstr_size) {
        int serr = SSL_get_error(rktrans->rktrans_ssl, ret);
        int serr2;

        switch (serr) {
        case SSL_ERROR_WANT_READ:
                rd_kafka_transport_poll_set(rktrans, POLLIN);
                break;

        case SSL_ERROR_WANT_WRITE:
                rd_kafka_transport_set_blocked(rktrans, rd_true);
                rd_kafka_transport_poll_set(rktrans, POLLOUT);
                break;

        case SSL_ERROR_SYSCALL:
                serr2 = ERR_peek_error();
                if (serr2)
                        rd_kafka_ssl_error(NULL, rktrans->rktrans_rkb, errstr,
                                           errstr_size);
                else if (!rd_socket_errno) {
                        rd_rkb_dbg(rktrans->rktrans_rkb, BROKER, "SOCKET",
                                   "Disconnected: connection closed by "
                                   "peer");
                        rd_snprintf(errstr, errstr_size, "Disconnected");
                } else if (rd_socket_errno == ECONNRESET) {
                        rd_rkb_dbg(rktrans->rktrans_rkb, BROKER, "SOCKET",
                                   "Disconnected: connection reset by peer");
                        rd_snprintf(errstr, errstr_size, "Disconnected");
                } else
                        rd_snprintf(errstr, errstr_size,
                                    "SSL transport error: %s",
                                    rd_strerror(rd_socket_errno));
                return -1;

        case SSL_ERROR_ZERO_RETURN:
                rd_rkb_dbg(rktrans->rktrans_rkb, BROKER, "SOCKET",
                           "Disconnected: SSL connection closed by peer");
                rd_snprintf(errstr, errstr_size, "Disconnected");
                return -1;

        default:
                rd_kafka_ssl_error(NULL, rktrans->rktrans_rkb, errstr,
                                   errstr_size);
                return -1;
        }

        return 0;
}

ssize_t rd_kafka_transport_ssl_send(rd_kafka_transport_t *rktrans,
                                    rd_slice_t *slice,
                                    char *errstr,
                                    size_t errstr_size) {
        ssize_t sum = 0;
        const void *p;
        size_t rlen;

        rd_kafka_transport_ssl_clear_error(rktrans);

        while ((rlen = rd_slice_peeker(slice, &p))) {
                int r;
                size_t r2;

                r = SSL_write(rktrans->rktrans_ssl, p, (int)rlen);

                if (unlikely(r <= 0)) {
                        if (rd_kafka_transport_ssl_io_update(rktrans, r, errstr,
                                                             errstr_size) == -1)
                                return -1;
                        else
                                return sum;
                }

                /* Update buffer read position */
                r2 = rd_slice_read(slice, NULL, (size_t)r);
                rd_assert((size_t)r == r2 &&
                          *"BUG: wrote more bytes than available in slice");


                sum += r;
                /* FIXME: remove this and try again immediately and let
                 *        the next SSL_write() call fail instead? */
                if ((size_t)r < rlen)
                        break;
        }
        return sum;
}

ssize_t rd_kafka_transport_ssl_recv(rd_kafka_transport_t *rktrans,
                                    rd_buf_t *rbuf,
                                    char *errstr,
                                    size_t errstr_size) {
        ssize_t sum = 0;
        void *p;
        size_t len;

        while ((len = rd_buf_get_writable(rbuf, &p))) {
                int r;

                rd_kafka_transport_ssl_clear_error(rktrans);

                r = SSL_read(rktrans->rktrans_ssl, p, (int)len);

                if (unlikely(r <= 0)) {
                        if (rd_kafka_transport_ssl_io_update(rktrans, r, errstr,
                                                             errstr_size) == -1)
                                return -1;
                        else
                                return sum;
                }

                VALGRIND_MAKE_MEM_DEFINED(p, r);

                /* Update buffer write position */
                rd_buf_write(rbuf, NULL, (size_t)r);

                sum += r;

                /* FIXME: remove this and try again immediately and let
                 *        the next SSL_read() call fail instead? */
                if ((size_t)r < len)
                        break;
        }
        return sum;
}


/**
 * OpenSSL password query callback
 *
 * Locality: application thread
 */
static int rd_kafka_transport_ssl_passwd_cb(char *buf,
                                            int size,
                                            int rwflag,
                                            void *userdata) {
        rd_kafka_t *rk = userdata;
        int pwlen;

        rd_kafka_dbg(rk, SECURITY, "SSLPASSWD",
                     "Private key requires password");

        if (!rk->rk_conf.ssl.key_password) {
                rd_kafka_log(rk, LOG_WARNING, "SSLPASSWD",
                             "Private key requires password but "
                             "no password configured (ssl.key.password)");
                return -1;
        }


        pwlen = (int)strlen(rk->rk_conf.ssl.key_password);
        memcpy(buf, rk->rk_conf.ssl.key_password, RD_MIN(pwlen, size));

        return pwlen;
}


/**
 * @brief OpenSSL callback to perform additional broker certificate
 *        verification and validation.
 *
 * @return 1 on success when the broker certificate
 *         is valid and 0 when the certificate is not valid.
 *
 * @sa SSL_CTX_set_verify()
 */
static int rd_kafka_transport_ssl_cert_verify_cb(int preverify_ok,
                                                 X509_STORE_CTX *x509_ctx) {
        rd_kafka_transport_t *rktrans = rd_kafka_curr_transport;
        rd_kafka_broker_t *rkb;
        rd_kafka_t *rk;
        X509 *cert;
        char *buf = NULL;
        int buf_size;
        int depth;
        int x509_orig_error, x509_error;
        char errstr[512];
        int ok;

        rd_assert(rktrans != NULL);
        rkb = rktrans->rktrans_rkb;
        rk  = rkb->rkb_rk;

        cert = X509_STORE_CTX_get_current_cert(x509_ctx);
        if (!cert) {
                rd_rkb_log(rkb, LOG_ERR, "SSLCERTVRFY",
                           "Failed to get current certificate to verify");
                return 0;
        }

        depth = X509_STORE_CTX_get_error_depth(x509_ctx);

        x509_orig_error = x509_error = X509_STORE_CTX_get_error(x509_ctx);

        buf_size = i2d_X509(cert, (unsigned char **)&buf);
        if (buf_size < 0 || !buf) {
                rd_rkb_log(rkb, LOG_ERR, "SSLCERTVRFY",
                           "Unable to convert certificate to X509 format");
                return 0;
        }

        *errstr = '\0';

        /* Call application's verification callback. */
        ok = rk->rk_conf.ssl.cert_verify_cb(
            rk, rkb->rkb_nodename, rkb->rkb_nodeid, &x509_error, depth, buf,
            (size_t)buf_size, errstr, sizeof(errstr), rk->rk_conf.opaque);

        OPENSSL_free(buf);

        if (!ok) {
                char subject[128];
                char issuer[128];

                X509_NAME_oneline(X509_get_subject_name(cert), subject,
                                  sizeof(subject));
                X509_NAME_oneline(X509_get_issuer_name(cert), issuer,
                                  sizeof(issuer));
                rd_rkb_log(rkb, LOG_ERR, "SSLCERTVRFY",
                           "Certificate (subject=%s, issuer=%s) verification "
                           "callback failed: %s",
                           subject, issuer, errstr);

                X509_STORE_CTX_set_error(x509_ctx, x509_error);

                return 0; /* verification failed */
        }

        /* Clear error */
        if (x509_orig_error != 0 && x509_error == 0)
                X509_STORE_CTX_set_error(x509_ctx, 0);

        return 1; /* verification successful */
}

/**
 * @brief Set TLSEXT hostname for SNI and optionally enable
 *        SSL endpoint identification verification.
 *
 * @returns 0 on success or -1 on error.
 */
static int rd_kafka_transport_ssl_set_endpoint_id(rd_kafka_transport_t *rktrans,
                                                  char *errstr,
                                                  size_t errstr_size) {
        char name[RD_KAFKA_NODENAME_SIZE];
        char *t;

        rd_kafka_broker_lock(rktrans->rktrans_rkb);
        rd_snprintf(name, sizeof(name), "%s",
                    rktrans->rktrans_rkb->rkb_nodename);
        rd_kafka_broker_unlock(rktrans->rktrans_rkb);

        /* Remove ":9092" port suffix from nodename */
        if ((t = strrchr(name, ':')))
                *t = '\0';

#if (OPENSSL_VERSION_NUMBER >= 0x0090806fL) && !defined(OPENSSL_NO_TLSEXT)
        /* If non-numerical hostname, send it for SNI */
        if (!(/*ipv6*/ (strchr(name, ':') &&
                        strspn(name, "0123456789abcdefABCDEF:.[]%") ==
                            strlen(name)) ||
              /*ipv4*/ strspn(name, "0123456789.") == strlen(name)) &&
            !SSL_set_tlsext_host_name(rktrans->rktrans_ssl, name))
                goto fail;
#endif

        if (rktrans->rktrans_rkb->rkb_rk->rk_conf.ssl.endpoint_identification ==
            RD_KAFKA_SSL_ENDPOINT_ID_NONE)
                return 0;

#if OPENSSL_VERSION_NUMBER >= 0x10100000 && !defined(OPENSSL_IS_BORINGSSL)
        if (!SSL_set1_host(rktrans->rktrans_ssl, name))
                goto fail;
#elif OPENSSL_VERSION_NUMBER >= 0x1000200fL /* 1.0.2 */
        {
                X509_VERIFY_PARAM *param;

                param = SSL_get0_param(rktrans->rktrans_ssl);

                if (!X509_VERIFY_PARAM_set1_host(param, name,
                                                 strnlen(name, sizeof(name))))
                        goto fail;
        }
#else
        rd_snprintf(errstr, errstr_size,
                    "Endpoint identification not supported on this "
                    "OpenSSL version (0x%lx)",
                    OPENSSL_VERSION_NUMBER);
        return -1;
#endif

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "ENDPOINT",
                   "Enabled endpoint identification using hostname %s", name);

        return 0;

fail:
        rd_kafka_ssl_error(NULL, rktrans->rktrans_rkb, errstr, errstr_size);
        return -1;
}


/**
 * @brief Set up SSL for a newly connected connection
 *
 * @returns -1 on failure, else 0.
 */
int rd_kafka_transport_ssl_connect(rd_kafka_broker_t *rkb,
                                   rd_kafka_transport_t *rktrans,
                                   char *errstr,
                                   size_t errstr_size) {
        int r;

        rktrans->rktrans_ssl = SSL_new(rkb->rkb_rk->rk_conf.ssl.ctx);
        if (!rktrans->rktrans_ssl)
                goto fail;

        if (!SSL_set_fd(rktrans->rktrans_ssl, (int)rktrans->rktrans_s))
                goto fail;

        if (rd_kafka_transport_ssl_set_endpoint_id(rktrans, errstr,
                                                   errstr_size) == -1)
                return -1;

        rd_kafka_transport_ssl_clear_error(rktrans);

        r = SSL_connect(rktrans->rktrans_ssl);
        if (r == 1) {
                /* Connected, highly unlikely since this is a
                 * non-blocking operation. */
                rd_kafka_transport_connect_done(rktrans, NULL);
                return 0;
        }

        if (rd_kafka_transport_ssl_io_update(rktrans, r, errstr, errstr_size) ==
            -1)
                return -1;

        return 0;

fail:
        rd_kafka_ssl_error(NULL, rkb, errstr, errstr_size);
        return -1;
}


static RD_UNUSED int
rd_kafka_transport_ssl_io_event(rd_kafka_transport_t *rktrans, int events) {
        int r;
        char errstr[512];

        if (events & POLLOUT) {
                rd_kafka_transport_ssl_clear_error(rktrans);

                r = SSL_write(rktrans->rktrans_ssl, NULL, 0);
                if (rd_kafka_transport_ssl_io_update(rktrans, r, errstr,
                                                     sizeof(errstr)) == -1)
                        goto fail;
        }

        return 0;

fail:
        /* Permanent error */
        rd_kafka_broker_fail(rktrans->rktrans_rkb, LOG_ERR,
                             RD_KAFKA_RESP_ERR__TRANSPORT, "%s", errstr);
        return -1;
}


/**
 * @brief Verify SSL handshake was valid.
 */
static int rd_kafka_transport_ssl_verify(rd_kafka_transport_t *rktrans) {
        long int rl;
        X509 *cert;

        if (!rktrans->rktrans_rkb->rkb_rk->rk_conf.ssl.enable_verify)
                return 0;

#if OPENSSL_VERSION_NUMBER >= 0x30000000
        cert = SSL_get1_peer_certificate(rktrans->rktrans_ssl);
#else
        cert = SSL_get_peer_certificate(rktrans->rktrans_ssl);
#endif
        X509_free(cert);
        if (!cert) {
                rd_kafka_broker_fail(rktrans->rktrans_rkb, LOG_ERR,
                                     RD_KAFKA_RESP_ERR__SSL,
                                     "Broker did not provide a certificate");
                return -1;
        }

        if ((rl = SSL_get_verify_result(rktrans->rktrans_ssl)) != X509_V_OK) {
                rd_kafka_broker_fail(rktrans->rktrans_rkb, LOG_ERR,
                                     RD_KAFKA_RESP_ERR__SSL,
                                     "Failed to verify broker certificate: %s",
                                     X509_verify_cert_error_string(rl));
                return -1;
        }

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SSLVERIFY",
                   "Broker SSL certificate verified");
        return 0;
}

/**
 * @brief SSL handshake handling.
 * Call repeatedly (based on IO events) until handshake is done.
 *
 * @returns -1 on error, 0 if handshake is still in progress,
 *          or 1 on completion.
 */
int rd_kafka_transport_ssl_handshake(rd_kafka_transport_t *rktrans) {
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;
        char errstr[512];
        int r;

        r = SSL_do_handshake(rktrans->rktrans_ssl);
        if (r == 1) {
                /* SSL handshake done. Verify. */
                if (rd_kafka_transport_ssl_verify(rktrans) == -1)
                        return -1;

                rd_kafka_transport_connect_done(rktrans, NULL);
                return 1;

        } else if (rd_kafka_transport_ssl_io_update(rktrans, r, errstr,
                                                    sizeof(errstr)) == -1) {
                const char *extra       = "";
                rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR__SSL;

                if (strstr(errstr, "unexpected message"))
                        extra =
                            ": client SSL authentication might be "
                            "required (see ssl.key.location and "
                            "ssl.certificate.location and consult the "
                            "broker logs for more information)";
                else if (strstr(errstr,
                                "tls_process_server_certificate:"
                                "certificate verify failed") ||
                         strstr(errstr, "error:0A000086") /*openssl3*/ ||
                         strstr(errstr,
                                "get_server_certificate:"
                                "certificate verify failed"))
                        extra =
                            ": broker certificate could not be verified, "
                            "verify that ssl.ca.location is correctly "
                            "configured or root CA certificates are "
                            "installed"
#ifdef __APPLE__
                            " (brew install openssl)"
#elif defined(_WIN32)
                            " (add broker's CA certificate to the Windows "
                            "Root certificate store)"
#else
                            " (install ca-certificates package)"
#endif
                            ;
                else if (!strcmp(errstr, "Disconnected")) {
                        extra = ": connecting to a PLAINTEXT broker listener?";
                        /* Disconnects during handshake are most likely
                         * not due to SSL, but rather at the transport level */
                        err = RD_KAFKA_RESP_ERR__TRANSPORT;
                }

                rd_kafka_broker_fail(rkb, LOG_ERR, err,
                                     "SSL handshake failed: %s%s", errstr,
                                     extra);
                return -1;
        }

        return 0;
}



/**
 * @brief Parse a PEM-formatted string into an EVP_PKEY (PrivateKey) object.
 *
 * @param str Input PEM string, nul-terminated
 *
 * @remark This method does not provide automatic addition of PEM
 *         headers and footers.
 *
 * @returns a new EVP_PKEY on success or NULL on error.
 */
static EVP_PKEY *rd_kafka_ssl_PKEY_from_string(rd_kafka_t *rk,
                                               const char *str) {
        BIO *bio = BIO_new_mem_buf((void *)str, -1);
        EVP_PKEY *pkey;

        pkey = PEM_read_bio_PrivateKey(bio, NULL,
                                       rd_kafka_transport_ssl_passwd_cb, rk);

        BIO_free(bio);

        return pkey;
}

/**
 * Read a PEM formatted cert chain from BIO \p in into \p chainp .
 *
 * @param rk rdkafka instance.
 * @param in BIO to read from.
 * @param chainp Stack to push the certificates to.
 *
 * @return 0 on success, -1 on error.
 */
int rd_kafka_ssl_read_cert_chain_from_BIO(BIO *in,
                                          STACK_OF(X509) * chainp,
                                          pem_password_cb *password_cb,
                                          void *password_cb_opaque) {
        X509 *ca;
        int r, ret = 0;
        unsigned long err;
        while (1) {
                ca = X509_new();
                if (ca == NULL) {
                        rd_assert(!*"X509_new() allocation failed");
                }
                if (PEM_read_bio_X509(in, &ca, password_cb,
                                      password_cb_opaque) != NULL) {
                        r = sk_X509_push(chainp, ca);
                        if (!r) {
                                X509_free(ca);
                                ret = -1;
                                goto end;
                        }
                } else {
                        X509_free(ca);
                        break;
                }
        }
        /* When the while loop ends, it's usually just EOF. */
        err = ERR_peek_last_error();
        if (ERR_GET_LIB(err) == ERR_LIB_PEM &&
            ERR_GET_REASON(err) == PEM_R_NO_START_LINE)
                ret = 0;
        else
                ret = -1; /* some real error */
        ERR_clear_error();
end:
        return ret;
}

/**
 * @brief Parse a PEM-formatted string into an X509 object.
 *        Rest of CA chain is pushed to the \p chainp stack.
 *
 * @param str Input PEM string, nul-terminated.
 * @param chainp Stack to push the certificates to.
 *
 * @returns a new X509 on success or NULL on error.
 *
 * @remark When NULL is returned the chainp stack is not modified.
 */
static X509 *rd_kafka_ssl_X509_from_string(rd_kafka_t *rk,
                                           const char *str,
                                           STACK_OF(X509) * chainp) {
        BIO *bio = BIO_new_mem_buf((void *)str, -1);
        X509 *x509;

        x509 =
            PEM_read_bio_X509(bio, NULL, rd_kafka_transport_ssl_passwd_cb, rk);

        if (!x509) {
                BIO_free(bio);
                return NULL;
        }

        if (rd_kafka_ssl_read_cert_chain_from_BIO(
                bio, chainp, rd_kafka_transport_ssl_passwd_cb, rk) != 0) {
                /* Rest of the certificate is present,
                 * but couldn't be read,
                 * returning NULL as certificate cannot be verified
                 * without its chain. */
                rd_kafka_log(rk, LOG_WARNING, "SSL",
                             "Failed to read certificate chain from PEM. "
                             "Returning NULL certificate too.");
                X509_free(x509);
                BIO_free(bio);
                return NULL;
        }

        BIO_free(bio);
        return x509;
}


#ifdef _WIN32

/**
 * @brief Attempt load CA certificates from a Windows Certificate store.
 */
static int rd_kafka_ssl_win_load_cert_store(rd_kafka_t *rk,
                                            const char *ctx_identifier,
                                            SSL_CTX *ctx,
                                            const char *store_name) {
        HCERTSTORE w_store;
        PCCERT_CONTEXT w_cctx = NULL;
        X509_STORE *store;
        int fail_cnt = 0, cnt = 0;
        char errstr[256];
        wchar_t *wstore_name;
        size_t wsize = 0;
        errno_t werr;

        /* Convert store_name to wide-char */
        werr = mbstowcs_s(&wsize, NULL, 0, store_name, strlen(store_name));
        if (werr || wsize < 2 || wsize > 1000) {
                rd_kafka_log(
                    rk, LOG_ERR, "CERTSTORE",
                    "%s: Invalid Windows certificate store name: %.*s%s",
                    ctx_identifier, 30, store_name,
                    wsize < 2 ? " (empty)" : " (truncated)");
                return -1;
        }
        wstore_name = rd_alloca(sizeof(*wstore_name) * wsize);
        werr        = mbstowcs_s(NULL, wstore_name, wsize, store_name,
                                 strlen(store_name));
        rd_assert(!werr);

        w_store = CertOpenStore(CERT_STORE_PROV_SYSTEM, 0, 0,
                                CERT_SYSTEM_STORE_CURRENT_USER |
                                    CERT_STORE_READONLY_FLAG |
                                    CERT_STORE_OPEN_EXISTING_FLAG,
                                wstore_name);
        if (!w_store) {
                rd_kafka_log(
                    rk, LOG_ERR, "CERTSTORE",
                    "%s: Failed to open Windows certificate "
                    "%s store: %s",
                    ctx_identifier, store_name,
                    rd_strerror_w32(GetLastError(), errstr, sizeof(errstr)));
                return -1;
        }

        /* Get the OpenSSL trust store */
        store = SSL_CTX_get_cert_store(ctx);

        /* Enumerate the Windows certs */
        while ((w_cctx = CertEnumCertificatesInStore(w_store, w_cctx))) {
                X509 *x509;

                /* Parse Windows cert: DER -> X.509 */
                x509 = d2i_X509(NULL,
                                (const unsigned char **)&w_cctx->pbCertEncoded,
                                (long)w_cctx->cbCertEncoded);
                if (!x509) {
                        fail_cnt++;
                        continue;
                }

                /* Add cert to OpenSSL's trust store */
                if (!X509_STORE_add_cert(store, x509))
                        fail_cnt++;
                else
                        cnt++;

                X509_free(x509);
        }

        if (w_cctx)
                CertFreeCertificateContext(w_cctx);

        CertCloseStore(w_store, 0);

        rd_kafka_dbg(rk, SECURITY, "CERTSTORE",
                     "%s: %d certificate(s) successfully added from "
                     "Windows Certificate %s store, %d failed",
                     ctx_identifier, cnt, store_name, fail_cnt);

        if (cnt == 0 && fail_cnt > 0)
                return -1;

        return cnt;
}

/**
 * @brief Load certs from the configured CSV list of Windows Cert stores.
 *
 * @returns the number of successfully loaded certificates, or -1 on error.
 */
int rd_kafka_ssl_win_load_cert_stores(rd_kafka_t *rk,
                                      const char *ctx_identifier,
                                      SSL_CTX *ctx,
                                      const char *store_names) {
        char *s;
        int cert_cnt = 0, fail_cnt = 0;

        if (!store_names || !*store_names)
                return 0;

        rd_strdupa(&s, store_names);

        /* Parse CSV list ("Root,CA, , ,Something") and load
         * each store in order. */
        while (*s) {
                char *t;
                const char *store_name;
                int r;

                while (isspace((int)*s) || *s == ',')
                        s++;

                if (!*s)
                        break;

                store_name = s;

                t = strchr(s, (int)',');
                if (t) {
                        *t = '\0';
                        s  = t + 1;
                        for (; t >= store_name && isspace((int)*t); t--)
                                *t = '\0';
                } else {
                        s = "";
                }

                r = rd_kafka_ssl_win_load_cert_store(rk, ctx_identifier, ctx,
                                                     store_name);
                if (r != -1)
                        cert_cnt += r;
                else
                        fail_cnt++;
        }

        if (cert_cnt == 0 && fail_cnt > 0)
                return -1;

        return cert_cnt;
}
#endif /* MSC_VER */

/**
 * @brief Probe for a single \p path and if found and not an empty directory,
 *        set it on the \p ctx.
 *
 * @returns 0 if CA location was set with an error, 1 if it was set correctly,
 *          -1 if path should be skipped.
 */
static int rd_kafka_ssl_set_ca_path(rd_kafka_t *rk,
                                    const char *ctx_identifier,
                                    const char *path,
                                    SSL_CTX *ctx,
                                    rd_bool_t *is_dir) {
        if (!rd_file_stat(path, is_dir))
                return -1;

        if (*is_dir && rd_kafka_dir_is_empty(path))
                return -1;

        rd_kafka_dbg(rk, SECURITY, "CACERTS",
                     "Setting default CA certificate location for %s "
                     "to \"%s\"",
                     ctx_identifier, path);

        return SSL_CTX_load_verify_locations(ctx, *is_dir ? NULL : path,
                                             *is_dir ? path : NULL);
}

/**
 * @brief Probe for the system's CA certificate location and if found set it
 *        on the \p CTX.
 *
 * @returns 0 if CA location was set, else -1.
 */
int rd_kafka_ssl_probe_and_set_default_ca_location(rd_kafka_t *rk,
                                                   const char *ctx_identifier,
                                                   SSL_CTX *ctx) {
#if _WIN32
        /* No standard location on Windows, CA certs are in the ROOT store. */
        return -1;
#else
        /* The probe paths are based on:
         * https://www.happyassassin.net/posts/2015/01/12/a-note-about-ssltls-trusted-certificate-stores-and-platforms/
         * Golang's crypto probing paths:
         *   https://golang.org/search?q=certFiles   and certDirectories
         */
        static const char *paths[] = {
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/certs/ca-bundle.crt",
            "/etc/pki/tls/certs/ca-bundle.trust.crt",
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",

            "/etc/ssl/ca-bundle.pem",
            "/etc/pki/tls/cacert.pem",
            "/etc/ssl/cert.pem",
            "/etc/ssl/cacert.pem",

            "/etc/certs/ca-certificates.crt",
            "/etc/ssl/certs/ca-certificates.crt",

            "/etc/ssl/certs",

            "/usr/local/etc/ssl/cert.pem",
            "/usr/local/etc/ssl/cacert.pem",

            "/usr/local/etc/ssl/certs/cert.pem",
            "/usr/local/etc/ssl/certs/cacert.pem",

            /* BSD */
            "/usr/local/share/certs/ca-root-nss.crt",
            "/etc/openssl/certs/ca-certificates.crt",
#ifdef __APPLE__
            "/private/etc/ssl/cert.pem",
            "/private/etc/ssl/certs",
            "/usr/local/etc/openssl@1.1/cert.pem",
            "/usr/local/etc/openssl@1.0/cert.pem",
            "/usr/local/etc/openssl/certs",
            "/System/Library/OpenSSL",
#endif
#ifdef _AIX
            "/var/ssl/certs/ca-bundle.crt",
#endif
            NULL,
        };
        const char *path = NULL;
        int i;

        for (i = 0; (path = paths[i]); i++) {
                rd_bool_t is_dir;
                int r = rd_kafka_ssl_set_ca_path(rk, ctx_identifier, path, ctx,
                                                 &is_dir);
                if (r == -1)
                        continue;

                if (r != 1) {
                        char errstr[512];
                        /* Read error and clear the error stack */
                        rd_kafka_ssl_error(rk, NULL, errstr, sizeof(errstr));
                        rd_kafka_dbg(rk, SECURITY, "CACERTS",
                                     "Failed to set default CA certificate "
                                     "location to %s %s for %s: %s: skipping",
                                     is_dir ? "directory" : "file", path,
                                     ctx_identifier, errstr);
                        continue;
                }

                return 0;
        }

        rd_kafka_dbg(rk, SECURITY, "CACERTS",
                     "Unable to find any standard CA certificate"
                     "paths for %s: is the ca-certificates package installed?",
                     ctx_identifier);
        return -1;
#endif
}

/**
 * @brief Simple utility function to check if \p ca DN is matching
 *        any of the DNs in the \p ca_dns stack.
 */
static int rd_kafka_ssl_cert_issuer_match(STACK_OF(X509_NAME) * ca_dns,
                                          X509 *ca) {
        X509_NAME *issuer_dn = X509_get_issuer_name(ca);
        X509_NAME *dn;
        int i;

        for (i = 0; i < sk_X509_NAME_num(ca_dns); i++) {
                dn = sk_X509_NAME_value(ca_dns, i);
                if (0 == X509_NAME_cmp(dn, issuer_dn)) {
                        /* match found */
                        return 1;
                }
        }
        return 0;
}

/**
 * @brief callback function for SSL_CTX_set_cert_cb, see
 * https://docs.openssl.org/master/man3/SSL_CTX_set_cert_cb for details
 * of the callback function requirements.
 *
 * According to section 4.2.4 of RFC 8446:
 * The "certificate_authorities" extension is used to indicate the
 * certificate authorities (CAs) which an endpoint supports and which
 * SHOULD be used by the receiving endpoint to guide certificate
 * selection.
 *
 * We avoid sending a client certificate if the issuer doesn't match any DN
 * of server trusted certificate authorities (SSL_get_client_CA_list).
 * This is done to avoid sending a client certificate that would almost
 * certainly be rejected by the peer and would avoid successful
 * SASL_SSL authentication on the same connection in case
 * `ssl.client.auth=requested`.
 */
static int rd_kafka_ssl_cert_callback(SSL *ssl, void *arg) {
        rd_kafka_t *rk = arg;
        STACK_OF(X509_NAME) * ca_list;
        STACK_OF(X509) *certs = NULL;
        X509 *cert;
        int i;

        /* Get client cert from SSL connection */
        cert = SSL_get_certificate(ssl);
        if (cert == NULL) {
                /* If there's no client certificate,
                 * skip certificate issuer verification and
                 * avoid logging a warning. */
                return 1;
        }

        /* Get the accepted client CA list from the SSL connection, this
         * comes from the `certificate_authorities` field. */
        ca_list = SSL_get_client_CA_list(ssl);
        if (sk_X509_NAME_num(ca_list) < 1) {
                /* `certificate_authorities` is supported either
                 * in CertificateRequest (SSL <= 3, TLS <= 1.2)
                 * or as an extension (TLS >= 1.3). This should be always
                 * available, but in case it isn't, just send the certificate
                 * and let the server validate it. */
                return 1;
        }

        if (rd_kafka_ssl_cert_issuer_match(ca_list, cert)) {
                /* A match is found, use the certificate. */
                return 1;
        }

        /* Get client cert chain from SSL connection */
        SSL_get0_chain_certs(ssl, &certs);

        if (certs) {
                /* Check if there's a match in the CA list for
                 * each cert in the chain. */
                for (i = 0; i < sk_X509_num(certs); i++) {
                        cert = sk_X509_value(certs, i);
                        if (rd_kafka_ssl_cert_issuer_match(ca_list, cert)) {
                                /* A match is found, use the certificate. */
                                return 1;
                        }
                }
        }

        /* No match is found, which means they would almost certainly be
         * rejected by the peer.
         * We decide to send no certificates. */
        rd_kafka_log(rk, LOG_WARNING, "SSL",
                     "No matching issuer found in "
                     "server trusted certificate authorities, "
                     "not sending any client certificates");
        SSL_certs_clear(ssl);
        return 1;
}

/**
 * @brief Registers certificates, keys, etc, on the SSL_CTX
 *
 * @returns -1 on error, or 0 on success.
 */
static int rd_kafka_ssl_set_certs(rd_kafka_t *rk,
                                  SSL_CTX *ctx,
                                  char *errstr,
                                  size_t errstr_size) {
        rd_bool_t ca_probe   = rd_true;
        rd_bool_t check_pkey = rd_false;
        int r;

        /*
         * ssl_ca, ssl.ca.location, or Windows cert root store,
         * or default paths.
         */
        if (rk->rk_conf.ssl.ca) {
                /* CA certificate chain set with conf_set_ssl_cert() */
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading CA certificate(s) from memory");

                SSL_CTX_set_cert_store(ctx, rk->rk_conf.ssl.ca->store);

                /* OpenSSL takes ownership of the store */
                rk->rk_conf.ssl.ca->store = NULL;

                ca_probe = rd_false;

        } else {

                if (rk->rk_conf.ssl.ca_location &&
                    strcmp(rk->rk_conf.ssl.ca_location, "probe")) {
                        /* CA certificate location, either file or directory. */
                        int is_dir =
                            rd_kafka_path_is_dir(rk->rk_conf.ssl.ca_location);

                        rd_kafka_dbg(rk, SECURITY, "SSL",
                                     "Loading CA certificate(s) from %s %s",
                                     is_dir ? "directory" : "file",
                                     rk->rk_conf.ssl.ca_location);

                        r = SSL_CTX_load_verify_locations(
                            ctx, !is_dir ? rk->rk_conf.ssl.ca_location : NULL,
                            is_dir ? rk->rk_conf.ssl.ca_location : NULL);

                        if (r != 1) {
                                rd_snprintf(errstr, errstr_size,
                                            "ssl.ca.location failed: ");
                                return -1;
                        }

                        ca_probe = rd_false;
                }

                if (rk->rk_conf.ssl.ca_pem) {
                        /* CA as PEM string */
                        X509 *x509;
                        X509_STORE *store;
                        BIO *bio;
                        int cnt = 0;

                        /* Get the OpenSSL trust store */
                        store = SSL_CTX_get_cert_store(ctx);
                        rd_assert(store != NULL);

                        rd_kafka_dbg(rk, SECURITY, "SSL",
                                     "Loading CA certificate(s) from string");

                        bio =
                            BIO_new_mem_buf((void *)rk->rk_conf.ssl.ca_pem, -1);
                        rd_assert(bio != NULL);

                        /* Add all certificates to cert store */
                        while ((x509 = PEM_read_bio_X509(
                                    bio, NULL, rd_kafka_transport_ssl_passwd_cb,
                                    rk))) {
                                if (!X509_STORE_add_cert(store, x509)) {
                                        rd_snprintf(errstr, errstr_size,
                                                    "failed to add ssl.ca.pem "
                                                    "certificate "
                                                    "#%d to CA cert store: ",
                                                    cnt);
                                        X509_free(x509);
                                        BIO_free(bio);
                                        return -1;
                                }

                                X509_free(x509);
                                cnt++;
                        }

                        if (!BIO_eof(bio) || !cnt) {
                                rd_snprintf(errstr, errstr_size,
                                            "failed to read certificate #%d "
                                            "from ssl.ca.pem: "
                                            "not in PEM format?: ",
                                            cnt);
                                BIO_free(bio);
                                return -1;
                        }

                        BIO_free(bio);

                        rd_kafka_dbg(rk, SECURITY, "SSL",
                                     "Loaded %d CA certificate(s) from string",
                                     cnt);


                        ca_probe = rd_false;
                }
        }

        if (ca_probe) {
#ifdef _WIN32
                /* Attempt to load CA root certificates from the
                 * configured Windows certificate stores. */
                r = rd_kafka_ssl_win_load_cert_stores(
                    rk, "kafka", ctx, rk->rk_conf.ssl.ca_cert_stores);
                if (r == 0) {
                        rd_kafka_log(
                            rk, LOG_NOTICE, "CERTSTORE",
                            "No CA certificates loaded from "
                            "Windows certificate stores: "
                            "falling back to default OpenSSL CA paths");
                        r = -1;
                } else if (r == -1)
                        rd_kafka_log(
                            rk, LOG_NOTICE, "CERTSTORE",
                            "Failed to load CA certificates from "
                            "Windows certificate stores: "
                            "falling back to default OpenSSL CA paths");
#else
                r = -1;
#endif

                if ((rk->rk_conf.ssl.ca_location &&
                     !strcmp(rk->rk_conf.ssl.ca_location, "probe"))
#if WITH_STATIC_LIB_libcrypto
                    || r == -1
#endif
                ) {
                        /* If OpenSSL was linked statically there is a risk
                         * that the system installed CA certificate path
                         * doesn't match the cert path of OpenSSL.
                         * To circumvent this we check for the existence
                         * of standard CA certificate paths and use the
                         * first one that is found.
                         * Ignore failures. */
                        r = rd_kafka_ssl_probe_and_set_default_ca_location(
                            rk, "kafka", ctx);
                }

                if (r == -1) {
                        /* Use default CA certificate paths from linked OpenSSL:
                         * ignore failures */

                        r = SSL_CTX_set_default_verify_paths(ctx);
                        if (r != 1) {
                                char errstr2[512];
                                /* Read error and clear the error stack. */
                                rd_kafka_ssl_error(rk, NULL, errstr2,
                                                   sizeof(errstr2));
                                rd_kafka_dbg(
                                    rk, SECURITY, "SSL",
                                    "SSL_CTX_set_default_verify_paths() "
                                    "failed: %s: ignoring",
                                    errstr2);
                        }
                        r = 0;
                }
        }

        if (rk->rk_conf.ssl.crl_location) {
                rd_kafka_dbg(rk, SECURITY, "SSL", "Loading CRL from file %s",
                             rk->rk_conf.ssl.crl_location);

                r = SSL_CTX_load_verify_locations(
                    ctx, rk->rk_conf.ssl.crl_location, NULL);

                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.crl.location failed: ");
                        return -1;
                }


                rd_kafka_dbg(rk, SECURITY, "SSL", "Enabling CRL checks");

                X509_STORE_set_flags(SSL_CTX_get_cert_store(ctx),
                                     X509_V_FLAG_CRL_CHECK);
        }


        /*
         * ssl_cert, ssl.certificate.location and ssl.certificate.pem
         */
        if (rk->rk_conf.ssl.cert) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading public key from memory");

                rd_assert(rk->rk_conf.ssl.cert->x509);
                r = SSL_CTX_use_certificate(ctx, rk->rk_conf.ssl.cert->x509);
                if (r != 1) {
                        rd_snprintf(errstr, errstr_size, "ssl_cert failed: ");
                        return -1;
                }

                if (rk->rk_conf.ssl.cert->chain) {
                        r = SSL_CTX_set0_chain(ctx,
                                               rk->rk_conf.ssl.cert->chain);
                        if (r != 1) {
                                rd_snprintf(errstr, errstr_size,
                                            "ssl_cert failed: "
                                            "setting certificate chain: ");
                                return -1;
                        } else {
                                /* The chain is now owned by the CTX */
                                rk->rk_conf.ssl.cert->chain = NULL;
                        }
                }
        }

        if (rk->rk_conf.ssl.cert_location) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading public key from file %s",
                             rk->rk_conf.ssl.cert_location);

                r = SSL_CTX_use_certificate_chain_file(
                    ctx, rk->rk_conf.ssl.cert_location);

                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.certificate.location failed: ");
                        return -1;
                }
        }

        if (rk->rk_conf.ssl.cert_pem) {
                X509 *x509;
                STACK_OF(X509) *ca = sk_X509_new_null();
                if (!ca) {
                        rd_assert(!*"sk_X509_new_null() allocation failed");
                }

                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading public key from string");

                x509 = rd_kafka_ssl_X509_from_string(
                    rk, rk->rk_conf.ssl.cert_pem, ca);
                if (!x509) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.certificate.pem failed: "
                                    "not in PEM format?: ");
                        sk_X509_pop_free(ca, X509_free);
                        return -1;
                }

                r = SSL_CTX_use_certificate(ctx, x509);

                X509_free(x509);

                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.certificate.pem failed: "
                                    "setting main certificate: ");
                        sk_X509_pop_free(ca, X509_free);
                        return -1;
                }

                if (sk_X509_num(ca) == 0) {
                        sk_X509_pop_free(ca, X509_free);
                } else {
                        r = SSL_CTX_set0_chain(ctx, ca);
                        if (r != 1) {
                                rd_snprintf(errstr, errstr_size,
                                            "ssl.certificate.pem failed: "
                                            "setting certificate chain: ");
                                sk_X509_pop_free(ca, X509_free);
                                return -1;
                        }
                }
        }

        /*
         * ssl_key, ssl.key.location and ssl.key.pem
         */
        if (rk->rk_conf.ssl.key) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading private key file from memory");

                rd_assert(rk->rk_conf.ssl.key->pkey);
                r = SSL_CTX_use_PrivateKey(ctx, rk->rk_conf.ssl.key->pkey);
                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl_key (in-memory) failed: ");
                        return -1;
                }

                check_pkey = rd_true;
        }

        if (rk->rk_conf.ssl.key_location) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading private key file from %s",
                             rk->rk_conf.ssl.key_location);

                r = SSL_CTX_use_PrivateKey_file(
                    ctx, rk->rk_conf.ssl.key_location, SSL_FILETYPE_PEM);
                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.key.location failed: ");
                        return -1;
                }

                check_pkey = rd_true;
        }

        if (rk->rk_conf.ssl.key_pem) {
                EVP_PKEY *pkey;

                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading private key from string");

                pkey =
                    rd_kafka_ssl_PKEY_from_string(rk, rk->rk_conf.ssl.key_pem);
                if (!pkey) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.key.pem failed: "
                                    "not in PEM format?: ");
                        return -1;
                }

                r = SSL_CTX_use_PrivateKey(ctx, pkey);

                EVP_PKEY_free(pkey);

                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.key.pem failed: ");
                        return -1;
                }

                /* We no longer need the PEM key (it is cached in the CTX),
                 * clear its memory. */
                rd_kafka_desensitize_str(rk->rk_conf.ssl.key_pem);

                check_pkey = rd_true;
        }


        /*
         * ssl.keystore.location
         */
        if (rk->rk_conf.ssl.keystore_location) {
                EVP_PKEY *pkey     = NULL;
                X509 *cert         = NULL;
                STACK_OF(X509) *ca = NULL;
                BIO *bio;
                PKCS12 *p12;

                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading client's keystore file from %s",
                             rk->rk_conf.ssl.keystore_location);

                bio = BIO_new_file(rk->rk_conf.ssl.keystore_location, "rb");
                if (!bio) {
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to open ssl.keystore.location: "
                                    "%s: ",
                                    rk->rk_conf.ssl.keystore_location);
                        return -1;
                }

                p12 = d2i_PKCS12_bio(bio, NULL);
                if (!p12) {
                        BIO_free(bio);
                        rd_snprintf(errstr, errstr_size,
                                    "Error reading ssl.keystore.location "
                                    "PKCS#12 file: %s: ",
                                    rk->rk_conf.ssl.keystore_location);
                        return -1;
                }

                if (!PKCS12_parse(p12, rk->rk_conf.ssl.keystore_password, &pkey,
                                  &cert, &ca)) {
                        EVP_PKEY_free(pkey);
                        X509_free(cert);
                        PKCS12_free(p12);
                        BIO_free(bio);
                        if (ca != NULL)
                                sk_X509_pop_free(ca, X509_free);
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to parse ssl.keystore.location "
                                    "PKCS#12 file: %s: ",
                                    rk->rk_conf.ssl.keystore_location);
                        return -1;
                }

                PKCS12_free(p12);
                BIO_free(bio);

                r = SSL_CTX_use_cert_and_key(ctx, cert, pkey, ca, 1);
                RD_IF_FREE(cert, X509_free);
                RD_IF_FREE(pkey, EVP_PKEY_free);
                if (ca != NULL)
                        sk_X509_pop_free(ca, X509_free);
                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to use ssl.keystore.location: ");
                        return -1;
                }

                check_pkey = rd_true;
        }

#if WITH_SSL_ENGINE
        /*
         * If applicable, use OpenSSL engine to fetch SSL certificate.
         */
        if (rk->rk_conf.ssl.engine) {
                STACK_OF(X509_NAME) *cert_names = sk_X509_NAME_new_null();
                STACK_OF(X509_OBJECT) *roots =
                    X509_STORE_get0_objects(SSL_CTX_get_cert_store(ctx));
                X509 *x509     = NULL;
                EVP_PKEY *pkey = NULL;
                int i          = 0;
                for (i = 0; i < sk_X509_OBJECT_num(roots); i++) {
                        x509 = X509_OBJECT_get0_X509(
                            sk_X509_OBJECT_value(roots, i));

                        if (x509)
                                sk_X509_NAME_push(cert_names,
                                                  X509_get_subject_name(x509));
                }

                if (cert_names)
                        sk_X509_NAME_free(cert_names);

                x509 = NULL;
                r    = ENGINE_load_ssl_client_cert(
                    rk->rk_conf.ssl.engine, NULL, cert_names, &x509, &pkey,
                    NULL, NULL, rk->rk_conf.ssl.engine_callback_data);

                sk_X509_NAME_free(cert_names);
                if (r == -1 || !x509 || !pkey) {
                        X509_free(x509);
                        EVP_PKEY_free(pkey);
                        if (r == -1)
                                rd_snprintf(errstr, errstr_size,
                                            "OpenSSL "
                                            "ENGINE_load_ssl_client_cert "
                                            "failed: ");
                        else if (!x509)
                                rd_snprintf(errstr, errstr_size,
                                            "OpenSSL engine failed to "
                                            "load certificate: ");
                        else
                                rd_snprintf(errstr, errstr_size,
                                            "OpenSSL engine failed to "
                                            "load private key: ");

                        return -1;
                }

                r = SSL_CTX_use_certificate(ctx, x509);
                X509_free(x509);
                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to use SSL_CTX_use_certificate "
                                    "with engine: ");
                        EVP_PKEY_free(pkey);
                        return -1;
                }

                r = SSL_CTX_use_PrivateKey(ctx, pkey);
                EVP_PKEY_free(pkey);
                if (r != 1) {
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to use SSL_CTX_use_PrivateKey "
                                    "with engine: ");
                        return -1;
                }

                check_pkey = rd_true;
        }
#endif /*WITH_SSL_ENGINE*/

        /* Check that a valid private/public key combo was set. */
        if (check_pkey && SSL_CTX_check_private_key(ctx) != 1) {
                rd_snprintf(errstr, errstr_size, "Private key check failed: ");
                return -1;
        }

        /* Set client certificate callback to control the behaviour
         * of client certificate selection TLS handshake. */
        SSL_CTX_set_cert_cb(ctx, rd_kafka_ssl_cert_callback, rk);

        return 0;
}


/**
 * @brief Once per rd_kafka_t handle cleanup of OpenSSL
 *
 * @locality any thread
 *
 * @locks rd_kafka_wrlock() MUST be held
 */
void rd_kafka_ssl_ctx_term(rd_kafka_t *rk) {
        SSL_CTX_free(rk->rk_conf.ssl.ctx);
        rk->rk_conf.ssl.ctx = NULL;

#if WITH_SSL_ENGINE
        RD_IF_FREE(rk->rk_conf.ssl.engine, ENGINE_free);
#endif
}


#if WITH_SSL_ENGINE
/**
 * @brief Initialize and load OpenSSL engine, if configured.
 *
 * @returns true on success, false on error.
 */
static rd_bool_t
rd_kafka_ssl_ctx_init_engine(rd_kafka_t *rk, char *errstr, size_t errstr_size) {
        ENGINE *engine;

        /* OpenSSL loads an engine as dynamic id and stores it in
         * internal list, as per LIST_ADD command below. If engine
         * already exists in internal list, it is supposed to be
         * fetched using engine id.
         */
        engine = ENGINE_by_id(rk->rk_conf.ssl.engine_id);
        if (!engine) {
                engine = ENGINE_by_id("dynamic");
                if (!engine) {
                        rd_snprintf(errstr, errstr_size,
                                    "OpenSSL engine initialization failed in"
                                    " ENGINE_by_id: ");
                        return rd_false;
                }
        }

        if (!ENGINE_ctrl_cmd_string(engine, "SO_PATH",
                                    rk->rk_conf.ssl.engine_location, 0)) {
                ENGINE_free(engine);
                rd_snprintf(errstr, errstr_size,
                            "OpenSSL engine initialization failed in"
                            " ENGINE_ctrl_cmd_string SO_PATH: ");
                return rd_false;
        }

        if (!ENGINE_ctrl_cmd_string(engine, "LIST_ADD", "1", 0)) {
                ENGINE_free(engine);
                rd_snprintf(errstr, errstr_size,
                            "OpenSSL engine initialization failed in"
                            " ENGINE_ctrl_cmd_string LIST_ADD: ");
                return rd_false;
        }

        if (!ENGINE_ctrl_cmd_string(engine, "LOAD", NULL, 0)) {
                ENGINE_free(engine);
                rd_snprintf(errstr, errstr_size,
                            "OpenSSL engine initialization failed in"
                            " ENGINE_ctrl_cmd_string LOAD: ");
                return rd_false;
        }

        if (!ENGINE_init(engine)) {
                ENGINE_free(engine);
                rd_snprintf(errstr, errstr_size,
                            "OpenSSL engine initialization failed in"
                            " ENGINE_init: ");
                return rd_false;
        }

        rk->rk_conf.ssl.engine = engine;

        return rd_true;
}
#endif


#if OPENSSL_VERSION_NUMBER >= 0x30000000
/**
 * @brief Wrapper around OSSL_PROVIDER_unload() to expose a free(void*) API
 *        suitable for rd_list_t's free_cb.
 */
static void rd_kafka_ssl_OSSL_PROVIDER_free(void *ptr) {
        OSSL_PROVIDER *prov = ptr;
        (void)OSSL_PROVIDER_unload(prov);
}


/**
 * @brief Load OpenSSL 3.0.x providers specified in comma-separated string.
 *
 * @remark Only the error preamble/prefix is written here, the actual
 *         OpenSSL error is retrieved from the OpenSSL error stack by
 *         the caller.
 *
 * @returns rd_false on failure (errstr will be written to), or rd_true
 *          on successs.
 */
static rd_bool_t rd_kafka_ssl_ctx_load_providers(rd_kafka_t *rk,
                                                 const char *providers_csv,
                                                 char *errstr,
                                                 size_t errstr_size) {
        size_t provider_cnt, i;
        char **providers = rd_string_split(
            providers_csv, ',', rd_true /*skip empty*/, &provider_cnt);


        if (!providers || !provider_cnt) {
                rd_snprintf(errstr, errstr_size,
                            "ssl.providers expects a comma-separated "
                            "list of OpenSSL 3.0.x providers");
                if (providers)
                        rd_free(providers);
                return rd_false;
        }

        rd_list_init(&rk->rk_conf.ssl.loaded_providers, (int)provider_cnt,
                     rd_kafka_ssl_OSSL_PROVIDER_free);

        for (i = 0; i < provider_cnt; i++) {
                const char *provider = providers[i];
                OSSL_PROVIDER *prov;
                const char *buildinfo = NULL;
                OSSL_PARAM request[]  = {{"buildinfo", OSSL_PARAM_UTF8_PTR,
                                          (void *)&buildinfo, 0, 0},
                                         {NULL, 0, NULL, 0, 0}};

                prov = OSSL_PROVIDER_load(NULL, provider);
                if (!prov) {
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to load OpenSSL provider \"%s\": ",
                                    provider);
                        rd_free(providers);
                        return rd_false;
                }

                if (!OSSL_PROVIDER_get_params(prov, request))
                        buildinfo = "no buildinfo";

                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "OpenSSL provider \"%s\" loaded (%s)", provider,
                             buildinfo);

                rd_list_add(&rk->rk_conf.ssl.loaded_providers, prov);
        }

        rd_free(providers);

        return rd_true;
}
#endif



/**
 * @brief Once per rd_kafka_t handle initialization of OpenSSL
 *
 * @locality application thread
 *
 * @locks rd_kafka_wrlock() MUST be held
 */
int rd_kafka_ssl_ctx_init(rd_kafka_t *rk, char *errstr, size_t errstr_size) {
        int r;
        SSL_CTX *ctx = NULL;
        const char *linking =
#if WITH_STATIC_LIB_libcrypto
            "statically linked "
#else
            ""
#endif
            ;

#if OPENSSL_VERSION_NUMBER >= 0x10100000
        rd_kafka_dbg(rk, SECURITY, "OPENSSL",
                     "Using %sOpenSSL version %s "
                     "(0x%lx, librdkafka built with 0x%lx)",
                     linking, OpenSSL_version(OPENSSL_VERSION),
                     OpenSSL_version_num(), OPENSSL_VERSION_NUMBER);
#else
        rd_kafka_dbg(rk, SECURITY, "OPENSSL",
                     "librdkafka built with %sOpenSSL version 0x%lx", linking,
                     OPENSSL_VERSION_NUMBER);
#endif

        if (errstr_size > 0)
                errstr[0] = '\0';

#if OPENSSL_VERSION_NUMBER >= 0x30000000
        if (rk->rk_conf.ssl.providers &&
            !rd_kafka_ssl_ctx_load_providers(rk, rk->rk_conf.ssl.providers,
                                             errstr, errstr_size))
                goto fail;
#endif

#if WITH_SSL_ENGINE
        if (rk->rk_conf.ssl.engine_location && !rk->rk_conf.ssl.engine) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Loading OpenSSL engine from \"%s\"",
                             rk->rk_conf.ssl.engine_location);
                if (!rd_kafka_ssl_ctx_init_engine(rk, errstr, errstr_size))
                        goto fail;
        }
#endif

#if OPENSSL_VERSION_NUMBER >= 0x10100000
        ctx = SSL_CTX_new(TLS_client_method());
#else
        ctx = SSL_CTX_new(SSLv23_client_method());
#endif
        if (!ctx) {
                rd_snprintf(errstr, errstr_size, "SSL_CTX_new() failed: ");
                goto fail;
        }

#ifdef SSL_OP_NO_SSLv3
        /* Disable SSLv3 (unsafe) */
        SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv3);
#endif

        /* Key file password callback */
        SSL_CTX_set_default_passwd_cb(ctx, rd_kafka_transport_ssl_passwd_cb);
        SSL_CTX_set_default_passwd_cb_userdata(ctx, rk);

        /* Ciphers */
        if (rk->rk_conf.ssl.cipher_suites) {
                rd_kafka_dbg(rk, SECURITY, "SSL", "Setting cipher list: %s",
                             rk->rk_conf.ssl.cipher_suites);
                if (!SSL_CTX_set_cipher_list(ctx,
                                             rk->rk_conf.ssl.cipher_suites)) {
                        /* Set a string that will prefix the
                         * the OpenSSL error message (which is lousy)
                         * to make it more meaningful. */
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.cipher.suites failed: ");
                        goto fail;
                }
        }

        /* Set up broker certificate verification. */
        SSL_CTX_set_verify(ctx,
                           rk->rk_conf.ssl.enable_verify ? SSL_VERIFY_PEER
                                                         : SSL_VERIFY_NONE,
                           rk->rk_conf.ssl.cert_verify_cb
                               ? rd_kafka_transport_ssl_cert_verify_cb
                               : NULL);

#if OPENSSL_VERSION_NUMBER >= 0x1000200fL && !defined(LIBRESSL_VERSION_NUMBER)
        /* Curves */
        if (rk->rk_conf.ssl.curves_list) {
                rd_kafka_dbg(rk, SECURITY, "SSL", "Setting curves list: %s",
                             rk->rk_conf.ssl.curves_list);
                if (!SSL_CTX_set1_curves_list(ctx,
                                              rk->rk_conf.ssl.curves_list)) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.curves.list failed: ");
                        goto fail;
                }
        }

        /* Certificate signature algorithms */
        if (rk->rk_conf.ssl.sigalgs_list) {
                rd_kafka_dbg(rk, SECURITY, "SSL",
                             "Setting signature algorithms list: %s",
                             rk->rk_conf.ssl.sigalgs_list);
                if (!SSL_CTX_set1_sigalgs_list(ctx,
                                               rk->rk_conf.ssl.sigalgs_list)) {
                        rd_snprintf(errstr, errstr_size,
                                    "ssl.sigalgs.list failed: ");
                        goto fail;
                }
        }
#endif

        /* Register certificates, keys, etc. */
        if (rd_kafka_ssl_set_certs(rk, ctx, errstr, errstr_size) == -1)
                goto fail;


#ifdef SSL_OP_IGNORE_UNEXPECTED_EOF
        /* Ignore unexpected EOF error in OpenSSL 3.x, treating
         * it like a normal connection close even if
         * close_notify wasn't received.
         * see issue #4293 */
        SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

        SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE);

        rk->rk_conf.ssl.ctx = ctx;

        return 0;

fail:
        r = (int)strlen(errstr);
        /* If only the error preamble is provided in errstr and ending with
         * "....: ", then retrieve the last error from the OpenSSL error stack,
         * else treat the errstr as complete. */
        if (r > 2 && !strcmp(&errstr[r - 2], ": "))
                rd_kafka_ssl_error(rk, NULL, errstr + r,
                                   (int)errstr_size > r ? (int)errstr_size - r
                                                        : 0);
        RD_IF_FREE(ctx, SSL_CTX_free);
#if WITH_SSL_ENGINE
        RD_IF_FREE(rk->rk_conf.ssl.engine, ENGINE_free);
#endif
        rd_list_destroy(&rk->rk_conf.ssl.loaded_providers);

        return -1;
}


#if OPENSSL_VERSION_NUMBER < 0x10100000L
static RD_UNUSED void
rd_kafka_transport_ssl_lock_cb(int mode, int i, const char *file, int line) {
        if (mode & CRYPTO_LOCK)
                mtx_lock(&rd_kafka_ssl_locks[i]);
        else
                mtx_unlock(&rd_kafka_ssl_locks[i]);
}
#endif

static RD_UNUSED unsigned long rd_kafka_transport_ssl_threadid_cb(void) {
#ifdef _WIN32
        /* Windows makes a distinction between thread handle
         * and thread id, which means we can't use the
         * thrd_current() API that returns the handle. */
        return (unsigned long)GetCurrentThreadId();
#else
        return (unsigned long)(intptr_t)thrd_current();
#endif
}

#ifdef HAVE_OPENSSL_CRYPTO_THREADID_SET_CALLBACK
static void
rd_kafka_transport_libcrypto_THREADID_callback(CRYPTO_THREADID *id) {
        unsigned long thread_id = rd_kafka_transport_ssl_threadid_cb();

        CRYPTO_THREADID_set_numeric(id, thread_id);
}
#endif

/**
 * @brief Global OpenSSL cleanup.
 */
void rd_kafka_ssl_term(void) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
        int i;

        if (CRYPTO_get_locking_callback() == &rd_kafka_transport_ssl_lock_cb) {
                CRYPTO_set_locking_callback(NULL);
#ifdef HAVE_OPENSSL_CRYPTO_THREADID_SET_CALLBACK
                CRYPTO_THREADID_set_callback(NULL);
#else
                CRYPTO_set_id_callback(NULL);
#endif

                for (i = 0; i < rd_kafka_ssl_locks_cnt; i++)
                        mtx_destroy(&rd_kafka_ssl_locks[i]);

                rd_free(rd_kafka_ssl_locks);
        }
#endif
}


/**
 * @brief Global (once per process) OpenSSL init.
 */
void rd_kafka_ssl_init(void) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L
        int i;

        if (!CRYPTO_get_locking_callback()) {
                rd_kafka_ssl_locks_cnt = CRYPTO_num_locks();
                rd_kafka_ssl_locks     = rd_malloc(rd_kafka_ssl_locks_cnt *
                                                   sizeof(*rd_kafka_ssl_locks));
                for (i = 0; i < rd_kafka_ssl_locks_cnt; i++)
                        mtx_init(&rd_kafka_ssl_locks[i], mtx_plain);

                CRYPTO_set_locking_callback(rd_kafka_transport_ssl_lock_cb);

#ifdef HAVE_OPENSSL_CRYPTO_THREADID_SET_CALLBACK
                CRYPTO_THREADID_set_callback(
                    rd_kafka_transport_libcrypto_THREADID_callback);
#else
                CRYPTO_set_id_callback(rd_kafka_transport_ssl_threadid_cb);
#endif
        }

        /* OPENSSL_init_ssl(3) and OPENSSL_init_crypto(3) say:
         * "As of version 1.1.0 OpenSSL will automatically allocate
         * all resources that it needs so no explicit initialisation
         * is required. Similarly it will also automatically
         * deinitialise as required."
         */
        SSL_load_error_strings();
        SSL_library_init();

        ERR_load_BIO_strings();
        ERR_load_crypto_strings();
        OpenSSL_add_all_algorithms();
#endif
}

int rd_kafka_ssl_hmac(rd_kafka_broker_t *rkb,
                      const EVP_MD *evp,
                      const rd_chariov_t *in,
                      const rd_chariov_t *salt,
                      int itcnt,
                      rd_chariov_t *out) {
        unsigned int ressize = 0;
        unsigned char tempres[EVP_MAX_MD_SIZE];
        unsigned char *saltplus;
        int i;

        /* U1   := HMAC(str, salt + INT(1)) */
        saltplus = rd_alloca(salt->size + 4);
        memcpy(saltplus, salt->ptr, salt->size);
        saltplus[salt->size]     = 0;
        saltplus[salt->size + 1] = 0;
        saltplus[salt->size + 2] = 0;
        saltplus[salt->size + 3] = 1;

        /* U1   := HMAC(str, salt + INT(1)) */
        if (!HMAC(evp, (const unsigned char *)in->ptr, (int)in->size, saltplus,
                  salt->size + 4, tempres, &ressize)) {
                rd_rkb_dbg(rkb, SECURITY, "SSLHMAC", "HMAC priming failed");
                return -1;
        }

        memcpy(out->ptr, tempres, ressize);

        /* Ui-1 := HMAC(str, Ui-2) ..  */
        for (i = 1; i < itcnt; i++) {
                unsigned char tempdest[EVP_MAX_MD_SIZE];
                int j;

                if (unlikely(!HMAC(evp, (const unsigned char *)in->ptr,
                                   (int)in->size, tempres, ressize, tempdest,
                                   NULL))) {
                        rd_rkb_dbg(rkb, SECURITY, "SSLHMAC",
                                   "Hi() HMAC #%d/%d failed", i, itcnt);
                        return -1;
                }

                /* U1 XOR U2 .. */
                for (j = 0; j < (int)ressize; j++) {
                        out->ptr[j] ^= tempdest[j];
                        tempres[j] = tempdest[j];
                }
        }

        out->size = ressize;

        return 0;
}
