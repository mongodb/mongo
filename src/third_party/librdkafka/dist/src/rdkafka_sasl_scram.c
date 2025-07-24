/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2017-2022, Magnus Edenhill
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
 * Builtin SASL SCRAM support when Cyrus SASL is not available
 */
#include "rdkafka_int.h"
#include "rdkafka_transport.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_sasl.h"
#include "rdkafka_sasl_int.h"
#include "rdrand.h"
#include "rdunittest.h"
#include "rdbase64.h"


#if WITH_SSL
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#else
#error "WITH_SSL (OpenSSL) is required for SASL SCRAM"
#endif


/**
 * @brief Per-connection state
 */
struct rd_kafka_sasl_scram_state {
        enum { RD_KAFKA_SASL_SCRAM_STATE_CLIENT_FIRST_MESSAGE,
               RD_KAFKA_SASL_SCRAM_STATE_SERVER_FIRST_MESSAGE,
               RD_KAFKA_SASL_SCRAM_STATE_CLIENT_FINAL_MESSAGE,
        } state;
        rd_chariov_t cnonce;         /* client c-nonce */
        rd_chariov_t first_msg_bare; /* client-first-message-bare */
        char *ServerSignatureB64;    /* ServerSignature in Base64 */
        const EVP_MD *evp;           /* Hash function pointer */
};


/**
 * @brief Close and free authentication state
 */
static void rd_kafka_sasl_scram_close(rd_kafka_transport_t *rktrans) {
        struct rd_kafka_sasl_scram_state *state = rktrans->rktrans_sasl.state;

        if (!state)
                return;

        RD_IF_FREE(state->cnonce.ptr, rd_free);
        RD_IF_FREE(state->first_msg_bare.ptr, rd_free);
        RD_IF_FREE(state->ServerSignatureB64, rd_free);
        rd_free(state);
        rktrans->rktrans_sasl.state = NULL;
}



/**
 * @brief Generates a nonce string (a random printable string)
 * @remark dst->ptr will be allocated and must be freed.
 */
static void rd_kafka_sasl_scram_generate_nonce(rd_chariov_t *dst) {
        int i;
        dst->size = 32;
        dst->ptr  = rd_malloc(dst->size + 1);
        for (i = 0; i < (int)dst->size; i++)
                dst->ptr[i] = (char)rd_jitter(0x2d /*-*/, 0x7e /*~*/);
        dst->ptr[i] = 0;
}


/**
 * @brief Parses inbuf for SCRAM attribute \p attr (e.g., 's')
 * @returns a newly allocated copy of the value, or NULL
 *          on failure in which case an error is written to \p errstr
 *          prefixed by \p description.
 */
static char *rd_kafka_sasl_scram_get_attr(const rd_chariov_t *inbuf,
                                          char attr,
                                          const char *description,
                                          char *errstr,
                                          size_t errstr_size) {
        size_t of = 0;

        for (of = 0; of < inbuf->size;) {
                const char *td;
                size_t len;

                /* Find next delimiter , (if any) */
                td = memchr(&inbuf->ptr[of], ',', inbuf->size - of);
                if (td)
                        len = (size_t)(td - &inbuf->ptr[of]);
                else
                        len = inbuf->size - of;

                /* Check if attr "x=" matches */
                if (inbuf->ptr[of] == attr && inbuf->size > of + 1 &&
                    inbuf->ptr[of + 1] == '=') {
                        char *ret;
                        of += 2; /* past = */
                        ret = rd_malloc(len - 2 + 1);
                        memcpy(ret, &inbuf->ptr[of], len - 2);
                        ret[len - 2] = '\0';
                        return ret;
                }

                /* Not the attr we are looking for, skip
                 * past the next delimiter and continue looking. */
                of += len + 1;
        }

        rd_snprintf(errstr, errstr_size, "%s: could not find attribute (%c)",
                    description, attr);
        return NULL;
}


/**
 * @brief Perform H(str) hash function and stores the result in \p out
 *        which must be at least EVP_MAX_MD_SIZE.
 * @returns 0 on success, else -1
 */
static int rd_kafka_sasl_scram_H(rd_kafka_transport_t *rktrans,
                                 const rd_chariov_t *str,
                                 rd_chariov_t *out) {

        rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.scram_H(
            (const unsigned char *)str->ptr, str->size,
            (unsigned char *)out->ptr);

        out->size = rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.scram_H_size;
        return 0;
}

/**
 * @brief Perform HMAC(key,str) and stores the result in \p out
 *        which must be at least EVP_MAX_MD_SIZE.
 * @returns 0 on success, else -1
 */
static int rd_kafka_sasl_scram_HMAC(rd_kafka_transport_t *rktrans,
                                    const rd_chariov_t *key,
                                    const rd_chariov_t *str,
                                    rd_chariov_t *out) {
        const EVP_MD *evp =
            rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.scram_evp;
        unsigned int outsize;

        if (!HMAC(evp, (const unsigned char *)key->ptr, (int)key->size,
                  (const unsigned char *)str->ptr, (int)str->size,
                  (unsigned char *)out->ptr, &outsize)) {
                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SCRAM",
                           "HMAC failed");
                return -1;
        }

        out->size = outsize;

        return 0;
}

/**
 * @brief Perform \p itcnt iterations of HMAC() on the given buffer \p in
 *        using \p salt, writing the output into \p out which must be
 *        at least EVP_MAX_MD_SIZE. Actual size is updated in \p *outsize.
 * @returns 0 on success, else -1
 */
static int rd_kafka_sasl_scram_Hi(rd_kafka_transport_t *rktrans,
                                  const rd_chariov_t *in,
                                  const rd_chariov_t *salt,
                                  int itcnt,
                                  rd_chariov_t *out) {
        rd_kafka_broker_t *rkb = rktrans->rktrans_rkb;
        const EVP_MD *evp =
            rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.scram_evp;
        return rd_kafka_ssl_hmac(rkb, evp, in, salt, itcnt, out);
}



/**
 * @returns a SASL value-safe-char encoded string, replacing "," and "="
 *          with their escaped counterparts in a newly allocated string.
 */
static char *rd_kafka_sasl_safe_string(const char *str) {
        char *safe = NULL, *d = NULL /*avoid warning*/;
        int pass;
        size_t len = 0;

        /* Pass #1: scan for needed length and allocate.
         * Pass #2: encode string */
        for (pass = 0; pass < 2; pass++) {
                const char *s;
                for (s = str; *s; s++) {
                        if (pass == 0) {
                                /* If this byte needs to be escaped then
                                 * 3 output bytes are needed instead of 1. */
                                len += (*s == ',' || *s == '=') ? 3 : 1;
                                continue;
                        }

                        if (*s == ',') {
                                *(d++) = '=';
                                *(d++) = '2';
                                *(d++) = 'C';
                        } else if (*s == '=') {
                                *(d++) = '=';
                                *(d++) = '3';
                                *(d++) = 'D';
                        } else
                                *(d++) = *s;
                }

                if (pass == 0)
                        d = safe = rd_malloc(len + 1);
        }

        rd_assert(d == safe + (int)len);
        *d = '\0';

        return safe;
}


/**
 * @brief Build client-final-message-without-proof
 * @remark out->ptr will be allocated and must be freed.
 */
static void rd_kafka_sasl_scram_build_client_final_message_wo_proof(
    struct rd_kafka_sasl_scram_state *state,
    const char *snonce,
    rd_chariov_t *out) {
        const char *attr_c = "biws"; /* base64 encode of "n,," */

        /*
         * client-final-message-without-proof =
         *            channel-binding "," nonce [","
         *            extensions]
         */
        out->size = strlen("c=,r=") + strlen(attr_c) + state->cnonce.size +
                    strlen(snonce);
        out->ptr = rd_malloc(out->size + 1);
        rd_snprintf(out->ptr, out->size + 1, "c=%s,r=%.*s%s", attr_c,
                    (int)state->cnonce.size, state->cnonce.ptr, snonce);
}


/**
 * @brief Build client-final-message
 * @returns -1 on error.
 */
static int rd_kafka_sasl_scram_build_client_final_message(
    rd_kafka_transport_t *rktrans,
    const rd_chariov_t *salt,
    const char *server_nonce,
    const rd_chariov_t *server_first_msg,
    int itcnt,
    rd_chariov_t *out) {
        struct rd_kafka_sasl_scram_state *state = rktrans->rktrans_sasl.state;
        rd_kafka_conf_t *conf        = &rktrans->rktrans_rkb->rkb_rk->rk_conf;
        rd_chariov_t SaslPassword    = RD_ZERO_INIT;
        rd_chariov_t SaltedPassword  = {.ptr = rd_alloca(EVP_MAX_MD_SIZE)};
        rd_chariov_t ClientKey       = {.ptr = rd_alloca(EVP_MAX_MD_SIZE)};
        rd_chariov_t ServerKey       = {.ptr = rd_alloca(EVP_MAX_MD_SIZE)};
        rd_chariov_t StoredKey       = {.ptr = rd_alloca(EVP_MAX_MD_SIZE)};
        rd_chariov_t AuthMessage     = RD_ZERO_INIT;
        rd_chariov_t ClientSignature = {.ptr = rd_alloca(EVP_MAX_MD_SIZE)};
        rd_chariov_t ServerSignature = {.ptr = rd_alloca(EVP_MAX_MD_SIZE)};
        const rd_chariov_t ClientKeyVerbatim = {.ptr  = "Client Key",
                                                .size = 10};
        const rd_chariov_t ServerKeyVerbatim = {.ptr  = "Server Key",
                                                .size = 10};
        rd_chariov_t ClientProof = {.ptr = rd_alloca(EVP_MAX_MD_SIZE)};
        rd_chariov_t client_final_msg_wo_proof;
        char *ClientProofB64;
        int i;

        mtx_lock(&conf->sasl.lock);
        rd_strdupa(&SaslPassword.ptr, conf->sasl.password);
        mtx_unlock(&conf->sasl.lock);
        SaslPassword.size = strlen(SaslPassword.ptr);

        /* Constructing the ClientProof attribute (p):
         *
         * p = Base64-encoded ClientProof
         * SaltedPassword  := Hi(Normalize(password), salt, i)
         * ClientKey       := HMAC(SaltedPassword, "Client Key")
         * StoredKey       := H(ClientKey)
         * AuthMessage     := client-first-message-bare + "," +
         *                    server-first-message + "," +
         *                    client-final-message-without-proof
         * ClientSignature := HMAC(StoredKey, AuthMessage)
         * ClientProof     := ClientKey XOR ClientSignature
         * ServerKey       := HMAC(SaltedPassword, "Server Key")
         * ServerSignature := HMAC(ServerKey, AuthMessage)
         */

        /* SaltedPassword  := Hi(Normalize(password), salt, i) */
        if (rd_kafka_sasl_scram_Hi(rktrans, &SaslPassword, salt, itcnt,
                                   &SaltedPassword) == -1)
                return -1;

        /* ClientKey       := HMAC(SaltedPassword, "Client Key") */
        if (rd_kafka_sasl_scram_HMAC(rktrans, &SaltedPassword,
                                     &ClientKeyVerbatim, &ClientKey) == -1)
                return -1;

        /* StoredKey       := H(ClientKey) */
        if (rd_kafka_sasl_scram_H(rktrans, &ClientKey, &StoredKey) == -1)
                return -1;

        /* client-final-message-without-proof */
        rd_kafka_sasl_scram_build_client_final_message_wo_proof(
            state, server_nonce, &client_final_msg_wo_proof);

        /* AuthMessage     := client-first-message-bare + "," +
         *                    server-first-message + "," +
         *                    client-final-message-without-proof */
        AuthMessage.size = state->first_msg_bare.size + 1 +
                           server_first_msg->size + 1 +
                           client_final_msg_wo_proof.size;
        AuthMessage.ptr = rd_alloca(AuthMessage.size + 1);
        rd_snprintf(AuthMessage.ptr, AuthMessage.size + 1, "%.*s,%.*s,%.*s",
                    (int)state->first_msg_bare.size, state->first_msg_bare.ptr,
                    (int)server_first_msg->size, server_first_msg->ptr,
                    (int)client_final_msg_wo_proof.size,
                    client_final_msg_wo_proof.ptr);

        /*
         * Calculate ServerSignature for later verification when
         * server-final-message is received.
         */

        /* ServerKey       := HMAC(SaltedPassword, "Server Key") */
        if (rd_kafka_sasl_scram_HMAC(rktrans, &SaltedPassword,
                                     &ServerKeyVerbatim, &ServerKey) == -1) {
                rd_free(client_final_msg_wo_proof.ptr);
                return -1;
        }

        /* ServerSignature := HMAC(ServerKey, AuthMessage) */
        if (rd_kafka_sasl_scram_HMAC(rktrans, &ServerKey, &AuthMessage,
                                     &ServerSignature) == -1) {
                rd_free(client_final_msg_wo_proof.ptr);
                return -1;
        }

        /* Store the Base64 encoded ServerSignature for quick comparison */
        state->ServerSignatureB64 = rd_base64_encode_str(&ServerSignature);
        if (state->ServerSignatureB64 == NULL) {
                rd_free(client_final_msg_wo_proof.ptr);
                return -1;
        }

        /*
         * Continue with client-final-message
         */

        /* ClientSignature := HMAC(StoredKey, AuthMessage) */
        if (rd_kafka_sasl_scram_HMAC(rktrans, &StoredKey, &AuthMessage,
                                     &ClientSignature) == -1) {
                rd_free(client_final_msg_wo_proof.ptr);
                return -1;
        }

        /* ClientProof     := ClientKey XOR ClientSignature */
        assert(ClientKey.size == ClientSignature.size);
        for (i = 0; i < (int)ClientKey.size; i++)
                ClientProof.ptr[i] = ClientKey.ptr[i] ^ ClientSignature.ptr[i];
        ClientProof.size = ClientKey.size;


        /* Base64 encoded ClientProof */
        ClientProofB64 = rd_base64_encode_str(&ClientProof);
        if (ClientProofB64 == NULL) {
                rd_free(client_final_msg_wo_proof.ptr);
                return -1;
        }

        /* Construct client-final-message */
        out->size = client_final_msg_wo_proof.size + strlen(",p=") +
                    strlen(ClientProofB64);
        out->ptr = rd_malloc(out->size + 1);

        rd_snprintf(out->ptr, out->size + 1, "%.*s,p=%s",
                    (int)client_final_msg_wo_proof.size,
                    client_final_msg_wo_proof.ptr, ClientProofB64);
        rd_free(ClientProofB64);
        rd_free(client_final_msg_wo_proof.ptr);

        return 0;
}


/**
 * @brief Handle first message from server
 *
 * Parse server response which looks something like:
 * "r=fyko+d2lbbFgONR....,s=QSXCR+Q6sek8bf92,i=4096"
 *
 * @returns -1 on error.
 */
static int
rd_kafka_sasl_scram_handle_server_first_message(rd_kafka_transport_t *rktrans,
                                                const rd_chariov_t *in,
                                                rd_chariov_t *out,
                                                char *errstr,
                                                size_t errstr_size) {
        struct rd_kafka_sasl_scram_state *state = rktrans->rktrans_sasl.state;
        char *server_nonce;
        rd_chariov_t salt_b64, salt;
        char *itcntstr;
        const char *endptr;
        int itcnt;
        char *attr_m;

        /* Mandatory future extension check */
        if ((attr_m = rd_kafka_sasl_scram_get_attr(in, 'm', NULL, NULL, 0))) {
                rd_snprintf(errstr, errstr_size,
                            "Unsupported mandatory SCRAM extension");
                rd_free(attr_m);
                return -1;
        }

        /* Server nonce */
        if (!(server_nonce = rd_kafka_sasl_scram_get_attr(
                  in, 'r', "Server nonce in server-first-message", errstr,
                  errstr_size)))
                return -1;

        if (strlen(server_nonce) <= state->cnonce.size ||
            strncmp(state->cnonce.ptr, server_nonce, state->cnonce.size)) {
                rd_snprintf(errstr, errstr_size,
                            "Server/client nonce mismatch in "
                            "server-first-message");
                rd_free(server_nonce);
                return -1;
        }

        /* Salt (Base64) */
        if (!(salt_b64.ptr = rd_kafka_sasl_scram_get_attr(
                  in, 's', "Salt in server-first-message", errstr,
                  errstr_size))) {
                rd_free(server_nonce);
                return -1;
        }
        salt_b64.size = strlen(salt_b64.ptr);

        /* Convert Salt to binary */
        if (rd_base64_decode(&salt_b64, &salt) == -1) {
                rd_snprintf(errstr, errstr_size,
                            "Invalid Base64 Salt in server-first-message");
                rd_free(server_nonce);
                rd_free(salt_b64.ptr);
                return -1;
        }
        rd_free(salt_b64.ptr);

        /* Iteration count (as string) */
        if (!(itcntstr = rd_kafka_sasl_scram_get_attr(
                  in, 'i', "Iteration count in server-first-message", errstr,
                  errstr_size))) {
                rd_free(server_nonce);
                rd_free(salt.ptr);
                return -1;
        }

        /* Iteration count (as int) */
        errno = 0;
        itcnt = (int)strtoul(itcntstr, (char **)&endptr, 10);
        if (itcntstr == endptr || *endptr != '\0' || errno != 0 ||
            itcnt > 1000000) {
                rd_snprintf(errstr, errstr_size,
                            "Invalid value (not integer or too large) "
                            "for Iteration count in server-first-message");
                rd_free(server_nonce);
                rd_free(salt.ptr);
                rd_free(itcntstr);
                return -1;
        }
        rd_free(itcntstr);

        /* Build client-final-message */
        if (rd_kafka_sasl_scram_build_client_final_message(
                rktrans, &salt, server_nonce, in, itcnt, out) == -1) {
                rd_snprintf(errstr, errstr_size,
                            "Failed to build SCRAM client-final-message");
                rd_free(salt.ptr);
                rd_free(server_nonce);
                return -1;
        }

        rd_free(server_nonce);
        rd_free(salt.ptr);

        return 0;
}

/**
 * @brief Handle server-final-message
 *
 *        This is the end of authentication and the SCRAM state
 *        will be freed at the end of this function regardless of
 *        authentication outcome.
 *
 * @returns -1 on failure
 */
static int
rd_kafka_sasl_scram_handle_server_final_message(rd_kafka_transport_t *rktrans,
                                                const rd_chariov_t *in,
                                                char *errstr,
                                                size_t errstr_size) {
        struct rd_kafka_sasl_scram_state *state = rktrans->rktrans_sasl.state;
        char *attr_v, *attr_e;

        if ((attr_e = rd_kafka_sasl_scram_get_attr(
                 in, 'e', "server-error in server-final-message", errstr,
                 errstr_size))) {
                /* Authentication failed */

                rd_snprintf(errstr, errstr_size,
                            "SASL SCRAM authentication failed: "
                            "broker responded with %s",
                            attr_e);
                rd_free(attr_e);
                return -1;

        } else if ((attr_v = rd_kafka_sasl_scram_get_attr(
                        in, 'v', "verifier in server-final-message", errstr,
                        errstr_size))) {
                rd_kafka_conf_t *conf;

                /* Authentication succesful on server,
                 * but we need to verify the ServerSignature too. */
                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY | RD_KAFKA_DBG_BROKER,
                           "SCRAMAUTH",
                           "SASL SCRAM authentication successful on server: "
                           "verifying ServerSignature");

                if (strcmp(attr_v, state->ServerSignatureB64)) {
                        rd_snprintf(errstr, errstr_size,
                                    "SASL SCRAM authentication failed: "
                                    "ServerSignature mismatch "
                                    "(server's %s != ours %s)",
                                    attr_v, state->ServerSignatureB64);
                        rd_free(attr_v);
                        return -1;
                }
                rd_free(attr_v);

                conf = &rktrans->rktrans_rkb->rkb_rk->rk_conf;

                mtx_lock(&conf->sasl.lock);
                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY | RD_KAFKA_DBG_BROKER,
                           "SCRAMAUTH", "Authenticated as %s using %s",
                           conf->sasl.username, conf->sasl.mechanisms);
                mtx_unlock(&conf->sasl.lock);

                rd_kafka_sasl_auth_done(rktrans);
                return 0;

        } else {
                rd_snprintf(errstr, errstr_size,
                            "SASL SCRAM authentication failed: "
                            "no verifier or server-error returned from broker");
                return -1;
        }
}



/**
 * @brief Build client-first-message
 */
static void
rd_kafka_sasl_scram_build_client_first_message(rd_kafka_transport_t *rktrans,
                                               rd_chariov_t *out) {
        char *sasl_username;
        struct rd_kafka_sasl_scram_state *state = rktrans->rktrans_sasl.state;
        rd_kafka_conf_t *conf = &rktrans->rktrans_rkb->rkb_rk->rk_conf;

        rd_kafka_sasl_scram_generate_nonce(&state->cnonce);

        mtx_lock(&conf->sasl.lock);
        sasl_username = rd_kafka_sasl_safe_string(conf->sasl.username);
        mtx_unlock(&conf->sasl.lock);

        out->size =
            strlen("n,,n=,r=") + strlen(sasl_username) + state->cnonce.size;
        out->ptr = rd_malloc(out->size + 1);

        rd_snprintf(out->ptr, out->size + 1, "n,,n=%s,r=%.*s", sasl_username,
                    (int)state->cnonce.size, state->cnonce.ptr);
        rd_free(sasl_username);

        /* Save client-first-message-bare (skip gs2-header) */
        state->first_msg_bare.size = out->size - 3;
        state->first_msg_bare.ptr =
            rd_memdup(out->ptr + 3, state->first_msg_bare.size);
}



/**
 * @brief SASL SCRAM client state machine
 * @returns -1 on failure (errstr set), else 0.
 */
static int rd_kafka_sasl_scram_fsm(rd_kafka_transport_t *rktrans,
                                   const rd_chariov_t *in,
                                   char *errstr,
                                   size_t errstr_size) {
        static const char *state_names[] = {
            "client-first-message",
            "server-first-message",
            "client-final-message",
        };
        struct rd_kafka_sasl_scram_state *state = rktrans->rktrans_sasl.state;
        rd_chariov_t out                        = RD_ZERO_INIT;
        int r                                   = -1;
        rd_ts_t ts_start                        = rd_clock();
        int prev_state                          = state->state;

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASLSCRAM",
                   "SASL SCRAM client in state %s", state_names[state->state]);

        switch (state->state) {
        case RD_KAFKA_SASL_SCRAM_STATE_CLIENT_FIRST_MESSAGE:
                rd_dassert(!in); /* Not expecting any server-input */

                rd_kafka_sasl_scram_build_client_first_message(rktrans, &out);
                state->state = RD_KAFKA_SASL_SCRAM_STATE_SERVER_FIRST_MESSAGE;
                break;


        case RD_KAFKA_SASL_SCRAM_STATE_SERVER_FIRST_MESSAGE:
                rd_dassert(in); /* Requires server-input */

                if (rd_kafka_sasl_scram_handle_server_first_message(
                        rktrans, in, &out, errstr, errstr_size) == -1)
                        return -1;

                state->state = RD_KAFKA_SASL_SCRAM_STATE_CLIENT_FINAL_MESSAGE;
                break;

        case RD_KAFKA_SASL_SCRAM_STATE_CLIENT_FINAL_MESSAGE:
                rd_dassert(in); /* Requires server-input */

                r = rd_kafka_sasl_scram_handle_server_final_message(
                    rktrans, in, errstr, errstr_size);
                break;
        }

        if (out.ptr) {
                r = rd_kafka_sasl_send(rktrans, out.ptr, (int)out.size, errstr,
                                       errstr_size);
                rd_free(out.ptr);
        }

        ts_start = (rd_clock() - ts_start) / 1000;
        if (ts_start >= 100)
                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SCRAM",
                           "SASL SCRAM state %s handled in %" PRId64 "ms",
                           state_names[prev_state], ts_start);


        return r;
}


/**
 * @brief Handle received frame from broker.
 */
static int rd_kafka_sasl_scram_recv(rd_kafka_transport_t *rktrans,
                                    const void *buf,
                                    size_t size,
                                    char *errstr,
                                    size_t errstr_size) {
        const rd_chariov_t in = {.ptr = (char *)buf, .size = size};
        return rd_kafka_sasl_scram_fsm(rktrans, &in, errstr, errstr_size);
}


/**
 * @brief Initialize and start SASL SCRAM (builtin) authentication.
 *
 * Returns 0 on successful init and -1 on error.
 *
 * @locality broker thread
 */
static int rd_kafka_sasl_scram_client_new(rd_kafka_transport_t *rktrans,
                                          const char *hostname,
                                          char *errstr,
                                          size_t errstr_size) {
        struct rd_kafka_sasl_scram_state *state;

        state        = rd_calloc(1, sizeof(*state));
        state->state = RD_KAFKA_SASL_SCRAM_STATE_CLIENT_FIRST_MESSAGE;
        rktrans->rktrans_sasl.state = state;

        /* Kick off the FSM */
        return rd_kafka_sasl_scram_fsm(rktrans, NULL, errstr, errstr_size);
}



/**
 * @brief Validate SCRAM config and look up the hash function
 */
static int rd_kafka_sasl_scram_conf_validate(rd_kafka_t *rk,
                                             char *errstr,
                                             size_t errstr_size) {
        const char *mech = rk->rk_conf.sasl.mechanisms;
        rd_bool_t both_set;

        mtx_lock(&rk->rk_conf.sasl.lock);
        both_set = rk->rk_conf.sasl.username && rk->rk_conf.sasl.password;
        mtx_unlock(&rk->rk_conf.sasl.lock);

        if (!both_set) {
                rd_snprintf(errstr, errstr_size,
                            "sasl.username and sasl.password must be set");
                return -1;
        }

        if (!strcmp(mech, "SCRAM-SHA-1")) {
                rk->rk_conf.sasl.scram_evp    = EVP_sha1();
                rk->rk_conf.sasl.scram_H      = SHA1;
                rk->rk_conf.sasl.scram_H_size = SHA_DIGEST_LENGTH;
        } else if (!strcmp(mech, "SCRAM-SHA-256")) {
                rk->rk_conf.sasl.scram_evp    = EVP_sha256();
                rk->rk_conf.sasl.scram_H      = SHA256;
                rk->rk_conf.sasl.scram_H_size = SHA256_DIGEST_LENGTH;
        } else if (!strcmp(mech, "SCRAM-SHA-512")) {
                rk->rk_conf.sasl.scram_evp    = EVP_sha512();
                rk->rk_conf.sasl.scram_H      = SHA512;
                rk->rk_conf.sasl.scram_H_size = SHA512_DIGEST_LENGTH;
        } else {
                rd_snprintf(errstr, errstr_size,
                            "Unsupported hash function: %s "
                            "(try SCRAM-SHA-512)",
                            mech);
                return -1;
        }

        return 0;
}



const struct rd_kafka_sasl_provider rd_kafka_sasl_scram_provider = {
    .name          = "SCRAM (builtin)",
    .client_new    = rd_kafka_sasl_scram_client_new,
    .recv          = rd_kafka_sasl_scram_recv,
    .close         = rd_kafka_sasl_scram_close,
    .conf_validate = rd_kafka_sasl_scram_conf_validate,
};



/**
 * @name Unit tests
 */

/**
 * @brief Verify that a random nonce is generated.
 */
static int unittest_scram_nonce(void) {
        rd_chariov_t out1 = RD_ZERO_INIT;
        rd_chariov_t out2 = RD_ZERO_INIT;

        rd_kafka_sasl_scram_generate_nonce(&out1);
        RD_UT_ASSERT(out1.size == 32, "Wrong size %d", (int)out1.size);

        rd_kafka_sasl_scram_generate_nonce(&out2);
        RD_UT_ASSERT(out1.size == 32, "Wrong size %d", (int)out2.size);

        RD_UT_ASSERT(memcmp(out1.ptr, out2.ptr, out1.size) != 0,
                     "Expected generate_nonce() to return a random nonce");

        rd_free(out1.ptr);
        rd_free(out2.ptr);

        RD_UT_PASS();
}


/**
 * @brief Verify that the safe string function does not overwrite memory.
 *        Needs to be run with ASAN (which is done in release-tests) for
 *        proper verification.
 */
static int unittest_scram_safe(void) {
        const char *inout[] = {
            "just a string",
            "just a string",

            "another,one,that,needs=escaping!",
            "another=2Cone=2Cthat=2Cneeds=3Descaping!",

            "overflow?============================",
            "overflow?=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D"
            "=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D=3D",

            "=3D=3D=3D the mind boggles",
            "=3D3D=3D3D=3D3D the mind boggles",

            NULL,
            NULL};
        int i;

        for (i = 0; inout[i]; i += 2) {
                char *out            = rd_kafka_sasl_safe_string(inout[i]);
                const char *expected = inout[i + 1];

                RD_UT_ASSERT(!strcmp(out, expected),
                             "Expected sasl_safe_string(%s) => %s, not %s\n",
                             inout[i], expected, out);

                rd_free(out);
        }

        RD_UT_PASS();
}


int unittest_scram(void) {
        int fails = 0;

        fails += unittest_scram_nonce();
        fails += unittest_scram_safe();

        return fails;
}
