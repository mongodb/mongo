/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2016 Magnus Edenhill
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
 * Impelements SASL Kerberos GSSAPI authentication client
 * using the native Win32 SSPI.
 */

#include "rdkafka_int.h"
#include "rdkafka_transport.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_sasl.h"
#include "rdkafka_sasl_int.h"


#include <stdio.h>
#include <windows.h>
#include <ntsecapi.h>

#define SECURITY_WIN32
#pragma comment(lib, "secur32.lib")
#include <sspi.h>


#define RD_KAFKA_SASL_SSPI_CTX_ATTRS                                           \
        (ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT |                     \
         ISC_REQ_SEQUENCE_DETECT | ISC_REQ_CONNECTION)


/* Default maximum kerberos token size for newer versions of Windows */
#define RD_KAFKA_SSPI_MAX_TOKEN_SIZE 48000


/**
 * @brief Per-connection SASL state
 */
typedef struct rd_kafka_sasl_win32_state_s {
        CredHandle *cred;
        CtxtHandle *ctx;
        wchar_t principal[512]; /* Broker service principal and hostname */
} rd_kafka_sasl_win32_state_t;


/**
 * @returns the string representation of a SECURITY_STATUS error code
 */
static const char *rd_kafka_sasl_sspi_err2str(SECURITY_STATUS sr) {
        switch (sr) {
        case SEC_E_INSUFFICIENT_MEMORY:
                return "Insufficient memory";
        case SEC_E_INTERNAL_ERROR:
                return "Internal error";
        case SEC_E_INVALID_HANDLE:
                return "Invalid handle";
        case SEC_E_INVALID_TOKEN:
                return "Invalid token";
        case SEC_E_LOGON_DENIED:
                return "Logon denied";
        case SEC_E_NO_AUTHENTICATING_AUTHORITY:
                return "No authority could be contacted for authentication.";
        case SEC_E_NO_CREDENTIALS:
                return "No credentials";
        case SEC_E_TARGET_UNKNOWN:
                return "Target unknown";
        case SEC_E_UNSUPPORTED_FUNCTION:
                return "Unsupported functionality";
        case SEC_E_WRONG_CREDENTIAL_HANDLE:
                return "The principal that received the authentication "
                       "request is not the same as the one passed "
                       "into  the pszTargetName parameter. "
                       "This indicates a failure in mutual "
                       "authentication.";
        default:
                return "(no string representation)";
        }
}


/**
 * @brief Create new CredHandle
 */
static CredHandle *rd_kafka_sasl_sspi_cred_new(rd_kafka_transport_t *rktrans,
                                               char *errstr,
                                               size_t errstr_size) {
        TimeStamp expiry = {0, 0};
        SECURITY_STATUS sr;
        CredHandle *cred = rd_calloc(1, sizeof(*cred));

        sr = AcquireCredentialsHandle(NULL, __TEXT("Kerberos"),
                                      SECPKG_CRED_OUTBOUND, NULL, NULL, NULL,
                                      NULL, cred, &expiry);

        if (sr != SEC_E_OK) {
                rd_free(cred);
                rd_snprintf(errstr, errstr_size,
                            "Failed to acquire CredentialsHandle: "
                            "error code %d",
                            sr);
                return NULL;
        }

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASL",
                   "Acquired Kerberos credentials handle (expiry in %d.%ds)",
                   expiry.u.HighPart, expiry.u.LowPart);

        return cred;
}


/**
 * @brief Start or continue SSPI-based authentication processing.
 */
static int rd_kafka_sasl_sspi_continue(rd_kafka_transport_t *rktrans,
                                       const void *inbuf,
                                       size_t insize,
                                       char *errstr,
                                       size_t errstr_size) {
        rd_kafka_sasl_win32_state_t *state = rktrans->rktrans_sasl.state;
        SecBufferDesc outbufdesc, inbufdesc;
        SecBuffer outsecbuf, insecbuf;
        BYTE outbuf[RD_KAFKA_SSPI_MAX_TOKEN_SIZE];
        TimeStamp lifespan = {0, 0};
        ULONG ret_ctxattrs;
        CtxtHandle *ctx;
        SECURITY_STATUS sr;

        if (inbuf) {
                if (insize > ULONG_MAX) {
                        rd_snprintf(errstr, errstr_size,
                                    "Input buffer length too large (%" PRIusz
                                    ") "
                                    "and would overflow",
                                    insize);
                        return -1;
                }

                inbufdesc.ulVersion = SECBUFFER_VERSION;
                inbufdesc.cBuffers  = 1;
                inbufdesc.pBuffers  = &insecbuf;

                insecbuf.cbBuffer   = (unsigned long)insize;
                insecbuf.BufferType = SECBUFFER_TOKEN;
                insecbuf.pvBuffer   = (void *)inbuf;
        }

        outbufdesc.ulVersion = SECBUFFER_VERSION;
        outbufdesc.cBuffers  = 1;
        outbufdesc.pBuffers  = &outsecbuf;

        outsecbuf.cbBuffer   = sizeof(outbuf);
        outsecbuf.BufferType = SECBUFFER_TOKEN;
        outsecbuf.pvBuffer   = outbuf;

        if (!(ctx = state->ctx)) {
                /* First time: allocate context handle
                 * which will be filled in by Initialize..() */
                ctx = rd_calloc(1, sizeof(*ctx));
        }

        sr = InitializeSecurityContext(
            state->cred, state->ctx, state->principal,
            RD_KAFKA_SASL_SSPI_CTX_ATTRS |
                (state->ctx ? 0 : ISC_REQ_MUTUAL_AUTH | ISC_REQ_IDENTIFY),
            0, SECURITY_NATIVE_DREP, inbuf ? &inbufdesc : NULL, 0, ctx,
            &outbufdesc, &ret_ctxattrs, &lifespan);

        if (!state->ctx)
                state->ctx = ctx;

        switch (sr) {
        case SEC_E_OK:
                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASLAUTH",
                           "Initialized security context");

                rktrans->rktrans_sasl.complete = 1;
                break;
        case SEC_I_CONTINUE_NEEDED:
                break;
        case SEC_I_COMPLETE_NEEDED:
        case SEC_I_COMPLETE_AND_CONTINUE:
                rd_snprintf(errstr, errstr_size,
                            "CompleteAuthToken (Digest auth, %d) "
                            "not implemented",
                            sr);
                return -1;
        case SEC_I_INCOMPLETE_CREDENTIALS:
                rd_snprintf(errstr, errstr_size,
                            "Incomplete credentials: "
                            "invalid or untrusted certificate");
                return -1;
        default:
                rd_snprintf(errstr, errstr_size,
                            "InitializeSecurityContext "
                            "failed: %s (0x%x)",
                            rd_kafka_sasl_sspi_err2str(sr), sr);
                return -1;
        }

        if (rd_kafka_sasl_send(rktrans, outsecbuf.pvBuffer, outsecbuf.cbBuffer,
                               errstr, errstr_size) == -1)
                return -1;

        return 0;
}


/**
 * @brief Sends the token response to the broker
 */
static int rd_kafka_sasl_win32_send_response(rd_kafka_transport_t *rktrans,
                                             char *errstr,
                                             size_t errstr_size,
                                             SecBuffer *server_token) {
        rd_kafka_sasl_win32_state_t *state = rktrans->rktrans_sasl.state;
        SECURITY_STATUS sr;
        SecBuffer in_buffer;
        SecBuffer out_buffer;
        SecBuffer buffers[4];
        SecBufferDesc buffer_desc;
        SecPkgContext_Sizes sizes;
        SecPkgCredentials_NamesA names;
        int send_response;
        size_t namelen;

        sr = QueryContextAttributes(state->ctx, SECPKG_ATTR_SIZES, &sizes);
        if (sr != SEC_E_OK) {
                rd_snprintf(errstr, errstr_size,
                            "Send response failed: %s (0x%x)",
                            rd_kafka_sasl_sspi_err2str(sr), sr);
                return -1;
        }

        RD_MEMZERO(names);
        sr = QueryCredentialsAttributesA(state->cred, SECPKG_CRED_ATTR_NAMES,
                                         &names);

        if (sr != SEC_E_OK) {
                rd_snprintf(errstr, errstr_size,
                            "Query credentials failed: %s (0x%x)",
                            rd_kafka_sasl_sspi_err2str(sr), sr);
                return -1;
        }

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASLAUTH",
                   "Sending response message for user: %s", names.sUserName);

        namelen = strlen(names.sUserName) + 1;
        if (namelen > ULONG_MAX) {
                rd_snprintf(errstr, errstr_size,
                            "User name length too large (%" PRIusz
                            ") "
                            "and would overflow");
                return -1;
        }

        in_buffer.pvBuffer = (char *)names.sUserName;
        in_buffer.cbBuffer = (unsigned long)namelen;

        buffer_desc.cBuffers  = 4;
        buffer_desc.pBuffers  = buffers;
        buffer_desc.ulVersion = SECBUFFER_VERSION;

        /* security trailer */
        buffers[0].cbBuffer   = sizes.cbSecurityTrailer;
        buffers[0].BufferType = SECBUFFER_TOKEN;
        buffers[0].pvBuffer   = rd_calloc(1, sizes.cbSecurityTrailer);

        /* protection level and buffer size received from the server */
        buffers[1].cbBuffer   = server_token->cbBuffer;
        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer   = rd_calloc(1, server_token->cbBuffer);
        memcpy(buffers[1].pvBuffer, server_token->pvBuffer,
               server_token->cbBuffer);

        /* user principal */
        buffers[2].cbBuffer   = in_buffer.cbBuffer;
        buffers[2].BufferType = SECBUFFER_DATA;
        buffers[2].pvBuffer   = rd_calloc(1, buffers[2].cbBuffer);
        memcpy(buffers[2].pvBuffer, in_buffer.pvBuffer, in_buffer.cbBuffer);

        /* padding */
        buffers[3].cbBuffer   = sizes.cbBlockSize;
        buffers[3].BufferType = SECBUFFER_PADDING;
        buffers[3].pvBuffer   = rd_calloc(1, buffers[2].cbBuffer);

        sr = EncryptMessage(state->ctx, KERB_WRAP_NO_ENCRYPT, &buffer_desc, 0);
        if (sr != SEC_E_OK) {
                rd_snprintf(errstr, errstr_size,
                            "Encrypt message failed: %s (0x%x)",
                            rd_kafka_sasl_sspi_err2str(sr), sr);

                FreeContextBuffer(in_buffer.pvBuffer);
                rd_free(buffers[0].pvBuffer);
                rd_free(buffers[1].pvBuffer);
                rd_free(buffers[2].pvBuffer);
                rd_free(buffers[3].pvBuffer);
                return -1;
        }

        out_buffer.cbBuffer = buffers[0].cbBuffer + buffers[1].cbBuffer +
                              buffers[2].cbBuffer + buffers[3].cbBuffer;

        out_buffer.pvBuffer =
            rd_calloc(1, buffers[0].cbBuffer + buffers[1].cbBuffer +
                             buffers[2].cbBuffer + buffers[3].cbBuffer);

        memcpy(out_buffer.pvBuffer, buffers[0].pvBuffer, buffers[0].cbBuffer);

        memcpy((unsigned char *)out_buffer.pvBuffer + (int)buffers[0].cbBuffer,
               buffers[1].pvBuffer, buffers[1].cbBuffer);

        memcpy((unsigned char *)out_buffer.pvBuffer + buffers[0].cbBuffer +
                   buffers[1].cbBuffer,
               buffers[2].pvBuffer, buffers[2].cbBuffer);

        memcpy((unsigned char *)out_buffer.pvBuffer + buffers[0].cbBuffer +
                   buffers[1].cbBuffer + buffers[2].cbBuffer,
               buffers[3].pvBuffer, buffers[3].cbBuffer);

        send_response =
            rd_kafka_sasl_send(rktrans, out_buffer.pvBuffer,
                               out_buffer.cbBuffer, errstr, errstr_size);

        FreeContextBuffer(in_buffer.pvBuffer);
        rd_free(out_buffer.pvBuffer);
        rd_free(buffers[0].pvBuffer);
        rd_free(buffers[1].pvBuffer);
        rd_free(buffers[2].pvBuffer);
        rd_free(buffers[3].pvBuffer);

        return send_response;
}


/**
 * @brief Unwrap and validate token response from broker.
 */
static int rd_kafka_sasl_win32_validate_token(rd_kafka_transport_t *rktrans,
                                              const void *inbuf,
                                              size_t insize,
                                              char *errstr,
                                              size_t errstr_size) {
        rd_kafka_sasl_win32_state_t *state = rktrans->rktrans_sasl.state;
        SecBuffer buffers[2];
        SecBufferDesc buffer_desc;
        SECURITY_STATUS sr;
        char supported;

        if (insize > ULONG_MAX) {
                rd_snprintf(errstr, errstr_size,
                            "Input buffer length too large (%" PRIusz
                            ") "
                            "and would overflow");
                return -1;
        }

        buffer_desc.cBuffers  = 2;
        buffer_desc.pBuffers  = buffers;
        buffer_desc.ulVersion = SECBUFFER_VERSION;

        buffers[0].cbBuffer   = (unsigned long)insize;
        buffers[0].BufferType = SECBUFFER_STREAM;
        buffers[0].pvBuffer   = (void *)inbuf;

        buffers[1].cbBuffer   = 0;
        buffers[1].BufferType = SECBUFFER_DATA;
        buffers[1].pvBuffer   = NULL;

        sr = DecryptMessage(state->ctx, &buffer_desc, 0, NULL);
        if (sr != SEC_E_OK) {
                rd_snprintf(errstr, errstr_size,
                            "Decrypt message failed: %s (0x%x)",
                            rd_kafka_sasl_sspi_err2str(sr), sr);
                return -1;
        }

        if (buffers[1].cbBuffer < 4) {
                rd_snprintf(errstr, errstr_size,
                            "Validate token: "
                            "invalid message");
                return -1;
        }

        supported = ((char *)buffers[1].pvBuffer)[0];
        if (!(supported & 1)) {
                rd_snprintf(errstr, errstr_size,
                            "Validate token: "
                            "server does not support layer");
                return -1;
        }

        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASLAUTH",
                   "Validated server token");

        return rd_kafka_sasl_win32_send_response(rktrans, errstr, errstr_size,
                                                 &buffers[1]);
}


/**
 * @brief Handle SASL frame received from broker.
 */
static int rd_kafka_sasl_win32_recv(struct rd_kafka_transport_s *rktrans,
                                    const void *buf,
                                    size_t size,
                                    char *errstr,
                                    size_t errstr_size) {
        rd_kafka_sasl_win32_state_t *state = rktrans->rktrans_sasl.state;

        if (rktrans->rktrans_sasl.complete) {

                if (size > 0) {
                        /* After authentication is done the broker will send
                         * back its token for us to verify.
                         * The client responds to the broker which will
                         * return an empty (size==0) frame that
                         * completes the authentication handshake.
                         * With legacy SASL framing the final empty token
                         * is not sent. */
                        int r;

                        r = rd_kafka_sasl_win32_validate_token(
                            rktrans, buf, size, errstr, errstr_size);

                        if (r == -1) {
                                rktrans->rktrans_sasl.complete = 0;
                                return r;
                        } else if (rktrans->rktrans_rkb->rkb_features &
                                   RD_KAFKA_FEATURE_SASL_AUTH_REQ) {
                                /* Kafka-framed handshake requires
                                 * one more back and forth. */
                                return r;
                        }

                        /* Legacy-framed handshake is done here */
                }

                /* Final ack from broker. */
                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY, "SASLAUTH",
                           "Authenticated");
                rd_kafka_sasl_auth_done(rktrans);
                return 0;
        }

        return rd_kafka_sasl_sspi_continue(rktrans, buf, size, errstr,
                                           errstr_size);
}


/**
 * @brief Decommission SSPI state
 */
static void rd_kafka_sasl_win32_close(rd_kafka_transport_t *rktrans) {
        rd_kafka_sasl_win32_state_t *state = rktrans->rktrans_sasl.state;

        if (!state)
                return;

        if (state->ctx) {
                DeleteSecurityContext(state->ctx);
                rd_free(state->ctx);
        }
        if (state->cred) {
                FreeCredentialsHandle(state->cred);
                rd_free(state->cred);
        }
        rd_free(state);
}


static int rd_kafka_sasl_win32_client_new(rd_kafka_transport_t *rktrans,
                                          const char *hostname,
                                          char *errstr,
                                          size_t errstr_size) {
        rd_kafka_t *rk = rktrans->rktrans_rkb->rkb_rk;
        rd_kafka_sasl_win32_state_t *state;

        if (strcmp(rk->rk_conf.sasl.mechanisms, "GSSAPI")) {
                rd_snprintf(errstr, errstr_size,
                            "SASL mechanism \"%s\" not supported on platform",
                            rk->rk_conf.sasl.mechanisms);
                return -1;
        }

        state                       = rd_calloc(1, sizeof(*state));
        rktrans->rktrans_sasl.state = state;

        _snwprintf(state->principal, RD_ARRAYSIZE(state->principal), L"%hs/%hs",
                   rktrans->rktrans_rkb->rkb_rk->rk_conf.sasl.service_name,
                   hostname);

        state->cred = rd_kafka_sasl_sspi_cred_new(rktrans, errstr, errstr_size);
        if (!state->cred)
                return -1;

        if (rd_kafka_sasl_sspi_continue(rktrans, NULL, 0, errstr,
                                        errstr_size) == -1)
                return -1;

        return 0;
}

/**
 * @brief Validate config
 */
static int rd_kafka_sasl_win32_conf_validate(rd_kafka_t *rk,
                                             char *errstr,
                                             size_t errstr_size) {
        if (!rk->rk_conf.sasl.service_name) {
                rd_snprintf(errstr, errstr_size,
                            "sasl.kerberos.service.name must be set");
                return -1;
        }

        return 0;
}

const struct rd_kafka_sasl_provider rd_kafka_sasl_win32_provider = {
    .name          = "Win32 SSPI",
    .client_new    = rd_kafka_sasl_win32_client_new,
    .recv          = rd_kafka_sasl_win32_recv,
    .close         = rd_kafka_sasl_win32_close,
    .conf_validate = rd_kafka_sasl_win32_conf_validate};
