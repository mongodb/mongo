
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/base/init.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <cstddef>
#include <memory>

#include "asio/detail/assert.hpp"

namespace asio {
namespace ssl {
namespace detail {

// Bring in the _attr UDL (defined in mongo::literals) needed by LOGV2_DEBUG.
// MSVC requires the operator to be reachable via unqualified lookup; GCC/Clang
// are more permissive.  Importing only the literals namespace keeps the
// pollution minimal.
using namespace mongo::literals;

/**
 * Start or continue SSL handshake.
 *
 * Must be called until HandshakeState::Done is returned.
 *
 * Return status code to indicate whether it needs more data or if data needs to be sent to the
 * other side.
 */
ssl_want SSLHandshakeManager::nextHandshake(asio::error_code& ec, HandshakeState* pHandshakeState) {
    ASIO_ASSERT(_mode != HandshakeMode::Unknown);
    ec = asio::error_code();
    *pHandshakeState = HandshakeState::Continue;

    if (_state == State::HandshakeStart) {
        ssl_want want;

        if (_mode == HandshakeMode::Server) {
            // ASIO will ask for the handshake to start when the input buffer is empty
            // but we want data first so tell ASIO to give us data
            if (_pInBuffer->empty()) {
                return ssl_want::want_input_and_retry;
            }

            startServerHandshake(ec);
            if (ec) {
                return ssl_want::want_nothing;
            }

            want = doServerHandshake(ec, pHandshakeState);
            if (ec) {
                return want;
            }

        } else {
            startClientHandshake(ec);
            if (ec) {
                return ssl_want::want_nothing;
            }

            want = doClientHandshake(ec, pHandshakeState);
            if (ec) {
                return want;
            }
        }

        setState(State::NeedMoreHandshakeData);

        return want;
    } else if (_state == State::NeedMoreHandshakeData) {
        return ssl_want::want_input_and_retry;
    } else {
        ssl_want want;

        if (_mode == HandshakeMode::Server) {
            want = doServerHandshake(ec, pHandshakeState);
        } else {
            want = doClientHandshake(ec, pHandshakeState);
        }

        if (ec) {
            return want;
        }

        if (want == ssl_want::want_nothing || *pHandshakeState == HandshakeState::Done) {
            LOGV2_DEBUG(7998001,
                        2,
                        "TLS client handshake complete",
                        "want"_attr = static_cast<int>(want),
                        "handshakeState"_attr =
                            (*pHandshakeState == HandshakeState::Done ? "Done" : "Continue"));
            setState(State::Done);
        } else {
            setState(State::NeedMoreHandshakeData);
        }

        return want;
    }
}

/**
 * Begin graceful SSL shutdown. Either:
 * - respond to already received alert signalling connection shutdown on remote side
 * - start SSL shutdown by signalling remote side
 */
ssl_want SSLHandshakeManager::beginShutdown(asio::error_code& ec) {
    ASIO_ASSERT(_mode != HandshakeMode::Unknown);
    _state = State::HandshakeStart;

    return startShutdown(ec);
}

/*
 * Injest data from ASIO that has been received.
 */
void SSLHandshakeManager::writeEncryptedData(const void* data, std::size_t length) {
    // We have more data, it may not be enough to decode. We will decide if we have enough on
    // the next nextHandshake call.
    if (_state != State::HandshakeStart) {
        setState(State::HaveEncryptedData);
    }

    _pInBuffer->append(data, length);
}


void SSLHandshakeManager::startServerHandshake(asio::error_code& ec) {
    TimeStamp lifetime;
    SECURITY_STATUS ss = AcquireCredentialsHandleW(NULL,
                                                   const_cast<LPWSTR>(UNISP_NAME),
                                                   SECPKG_CRED_INBOUND,
                                                   NULL,
                                                   _cred,
                                                   NULL,
                                                   NULL,
                                                   _phcred,
                                                   &lifetime);
    if (ss != SEC_E_OK) {
        ec = asio::error_code(ss, asio::error::get_ssl_category());
        return;
    }
}

void SSLHandshakeManager::startClientHandshake(asio::error_code& ec) {
    TimeStamp lifetime;
    SECURITY_STATUS ss = AcquireCredentialsHandleW(NULL,
                                                   const_cast<LPWSTR>(UNISP_NAME),
                                                   SECPKG_CRED_OUTBOUND,
                                                   NULL,
                                                   _cred,
                                                   NULL,
                                                   NULL,
                                                   _phcred,
                                                   &lifetime);

    if (ss != SEC_E_OK) {
        ec = asio::error_code(ss, asio::error::get_ssl_category());
        return;
    }
}

ssl_want SSLHandshakeManager::startShutdown(asio::error_code& ec) {
    DWORD shutdownCode = SCHANNEL_SHUTDOWN;

    std::array<SecBuffer, 1> inputBuffers;
    inputBuffers[0].cbBuffer = sizeof(shutdownCode);
    inputBuffers[0].BufferType = SECBUFFER_TOKEN;
    inputBuffers[0].pvBuffer = &shutdownCode;

    SecBufferDesc inputBufferDesc;
    inputBufferDesc.ulVersion = SECBUFFER_VERSION;
    inputBufferDesc.cBuffers = inputBuffers.size();
    inputBufferDesc.pBuffers = inputBuffers.data();

    SECURITY_STATUS ss = ApplyControlToken(_phctxt, &inputBufferDesc);

    // Accept SEC_I_CONTEXT_EXPIRED (0x00090317) as a success: Schannel returns this informational
    // code from ApplyControlToken when the context has already been marked expired (e.g. after
    // processing a TLS 1.3 NewSessionTicket).  Only negative SECURITY_STATUS values are real
    // errors — the same pattern used for ISC/ASC and DecryptMessage throughout this file.
    LOGV2_DEBUG(7998023,
                2,
                "TLS shutdown: ApplyControlToken result",
                "mode"_attr = (_mode == HandshakeMode::Server ? "server" : "client"),
                "status"_attr = ss);

    if (ss < SEC_E_OK) {
        ec = asio::error_code(ss, asio::error::get_ssl_category());
        return ssl_want::want_nothing;
    }

    TimeStamp lifetime;

    std::array<SecBuffer, 1> outputBuffers;
    outputBuffers[0].cbBuffer = 0;
    outputBuffers[0].BufferType = SECBUFFER_TOKEN;
    outputBuffers[0].pvBuffer = NULL;
    ContextBufferDeleter deleter(&outputBuffers[0].pvBuffer);

    SecBufferDesc outputBufferDesc;
    outputBufferDesc.ulVersion = SECBUFFER_VERSION;
    outputBufferDesc.cBuffers = outputBuffers.size();
    outputBufferDesc.pBuffers = outputBuffers.data();

    if (_mode == HandshakeMode::Server) {
        ULONG attribs = getServerFlags() | ASC_REQ_ALLOCATE_MEMORY;

        SECURITY_STATUS ss = AcceptSecurityContext(
            _phcred, _phctxt, NULL, attribs, 0, _phctxt, &outputBufferDesc, &attribs, &lifetime);

        LOGV2_DEBUG(7998024,
                    2,
                    "TLS shutdown: AcceptSecurityContext result",
                    "status"_attr = ss,
                    "outputBytes"_attr = outputBuffers[0].cbBuffer);

        // Accept SEC_I_CONTEXT_EXPIRED (0x00090317) as a success: it is an informational code
        // returned by ASC when the context is already in the "expired" state (i.e. we are
        // responding to a close_notify we already received).  The check `ss < SEC_E_OK` mirrors
        // the pattern used in decryptBuffer — only negative SECURITY_STATUS values are errors.
        if (ss < SEC_E_OK) {
            ec = asio::error_code(ss, asio::error::get_ssl_category());
            return ssl_want::want_nothing;
        }

        _pOutBuffer->reset();
        _pOutBuffer->append(outputBuffers[0].pvBuffer, outputBuffers[0].cbBuffer);

        if (outputBuffers[0].cbBuffer != 0) {
            ec = asio::error::eof;
            return ssl_want::want_output;
        } else {
            return ssl_want::want_nothing;
        }
    } else {
        ULONG ContextAttributes;
        DWORD sspiFlags = getClientFlags() | ISC_REQ_ALLOCATE_MEMORY;

        ss = InitializeSecurityContextW(_phcred,
                                        _phctxt,
                                        const_cast<SEC_WCHAR*>(_serverName.c_str()),
                                        sspiFlags,
                                        0,
                                        0,
                                        NULL,
                                        0,
                                        _phctxt,
                                        &outputBufferDesc,
                                        &ContextAttributes,
                                        &lifetime);

        LOGV2_DEBUG(7998025,
                    2,
                    "TLS shutdown: InitializeSecurityContext result",
                    "status"_attr = ss,
                    "outputBytes"_attr = outputBuffers[0].cbBuffer);

        // Accept SEC_I_CONTEXT_EXPIRED (0x00090317) as a success: it is an informational code
        // returned by ISC when the context is already in the "expired" state (i.e. we are
        // responding to a close_notify we already received).  The check `ss < SEC_E_OK` mirrors
        // the pattern used in decryptBuffer — only negative SECURITY_STATUS values are errors.
        if (ss < SEC_E_OK) {
            ec = asio::error_code(ss, asio::error::get_ssl_category());
            return ssl_want::want_nothing;
        }

        _pOutBuffer->reset();
        _pOutBuffer->append(outputBuffers[0].pvBuffer, outputBuffers[0].cbBuffer);

        if (outputBuffers[0].cbBuffer != 0) {
            ec = asio::error::eof;
            return ssl_want::want_output;
        } else {
            return ssl_want::want_nothing;
        }
    }

    return ssl_want::want_nothing;
}

ssl_want SSLHandshakeManager::doServerHandshake(asio::error_code& ec,
                                                HandshakeState* pHandshakeState) {
    TimeStamp lifetime;

    _pOutBuffer->resize(kDefaultBufferSize);
    _alertBuffer.resize(1024);

    std::array<SecBuffer, 2> outputBuffers;
    outputBuffers[0].cbBuffer = _pOutBuffer->size();
    outputBuffers[0].BufferType = SECBUFFER_TOKEN;
    outputBuffers[0].pvBuffer = _pOutBuffer->data();

    outputBuffers[1].cbBuffer = _alertBuffer.size();
    outputBuffers[1].BufferType = SECBUFFER_ALERT;
    outputBuffers[1].pvBuffer = _alertBuffer.data();

    SecBufferDesc outputBufferDesc;
    outputBufferDesc.ulVersion = SECBUFFER_VERSION;
    outputBufferDesc.cBuffers = outputBuffers.size();
    outputBufferDesc.pBuffers = outputBuffers.data();

    std::array<SecBuffer, 2> inputBuffers;
    inputBuffers[0].cbBuffer = _pInBuffer->size();
    inputBuffers[0].BufferType = SECBUFFER_TOKEN;
    inputBuffers[0].pvBuffer = _pInBuffer->data();

    inputBuffers[1].cbBuffer = 0;
    inputBuffers[1].BufferType = SECBUFFER_EMPTY;
    inputBuffers[1].pvBuffer = NULL;

    SecBufferDesc inputBufferDesc;
    inputBufferDesc.ulVersion = SECBUFFER_VERSION;
    inputBufferDesc.cBuffers = inputBuffers.size();
    inputBufferDesc.pBuffers = inputBuffers.data();

    ULONG attribs = getServerFlags();
    ULONG retAttribs = 0;

    SECURITY_STATUS ss = AcceptSecurityContext(_phcred,
                                               SecIsValidHandle(_phctxt) ? _phctxt : NULL,
                                               &inputBufferDesc,
                                               attribs,
                                               0,
                                               _phctxt,
                                               &outputBufferDesc,
                                               &retAttribs,
                                               &lifetime);

    if (ss < SEC_E_OK) {
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            // TODO: consider using SECBUFFER_MISSING and approriate optimizations
            return ssl_want::want_input_and_retry;
        }

        ec = asio::error_code(ss, asio::error::get_ssl_category());

        if ((retAttribs & ASC_RET_EXTENDED_ERROR) && (outputBuffers[1].cbBuffer > 0)) {
            _pOutBuffer->resize(outputBuffers[0].cbBuffer);

            // Tell ASIO we have something to send back the last data
            return ssl_want::want_output;
        }

        return ssl_want::want_nothing;
    }

    if (!_sni_set) {
        DWORD client_hello_size = _pInBuffer->size();
        DWORD sni_size = client_hello_size + 1;
        PBYTE sni_ptr = nullptr;

        SECURITY_STATUS status =
            _sslGetServerIdentityFn(_pInBuffer->data(), client_hello_size, &sni_ptr, &sni_size, 0);
        if (status != SEC_E_OK) {
            ec = asio::error_code(status, asio::error::get_ssl_category());
        } else if (sni_ptr == nullptr) {
            _sni = boost::none;
            _sni_set = true;
        } else {
            std::vector<BYTE> sni(sni_size);
            std::memcpy(sni.data(), sni_ptr, sni_size);
            sni.push_back('\0');
            _sni = sni;
            _sni_set = true;
        }
    }

    // ASC_RET_EXTENDED_ERROR is not support on Windows 7/Windows 2008 R2.
    // ASC_RET_MUTUAL_AUTH is not set since we do our own certificate validation later.
    invariant(attribs == (retAttribs | ASC_RET_EXTENDED_ERROR | ASC_RET_MUTUAL_AUTH));

    LOGV2_DEBUG(7998004,
                2,
                "TLS server ASC result",
                "ss"_attr = ss,
                "outputBytes"_attr = outputBuffers[0].cbBuffer,
                "inputSize"_attr = _pInBuffer->size());

    if (inputBuffers[1].BufferType == SECBUFFER_EXTRA && inputBuffers[1].cbBuffer > 0) {
        // SECBUFFER_EXTRA do not set pvBuffer, just cbBuffer.
        // cbBuffer tells us how much remaining in the buffer is extra
        _pExtraEncryptedBuffer->reset();
        _pExtraEncryptedBuffer->append(_pInBuffer->data() +
                                           (_pInBuffer->size() - inputBuffers[1].cbBuffer),
                                       inputBuffers[1].cbBuffer);
    }


    // Next, figure out if we need to send any data out
    bool needOutput{false};

    // Did AcceptSecurityContext say we need to continue or is it done but left data in the
    // output buffer then we need to sent the data out.
    if (SEC_I_CONTINUE_NEEDED == ss || SEC_I_COMPLETE_AND_CONTINUE == ss ||
        (SEC_E_OK == ss && outputBuffers[0].cbBuffer != 0)) {
        needOutput = true;
    }

    // Tell the reusable buffer size of the data written.
    _pOutBuffer->resize(outputBuffers[0].cbBuffer);

    // Reset the input buffer
    _pInBuffer->reset();

    // Check if we have any additional data
    if (!_pExtraEncryptedBuffer->empty()) {
        _pInBuffer->swap(*_pExtraEncryptedBuffer);
        _pExtraEncryptedBuffer->reset();

        // When doing the handshake and we have extra data, this means we have an incomplete tls
        // record and need more bytes to complete the tls record.
        setState(State::NeedMoreHandshakeData);
    }

    if (needOutput) {
        // If AcceptSecurityContext returns SEC_E_OK, then the handshake is done
        if (SEC_E_OK == ss && outputBuffers[0].cbBuffer != 0) {
            *pHandshakeState = HandshakeState::Done;

            // We have output, but no need to retry anymore
            return ssl_want::want_output;
        }

        return ssl_want::want_output_and_retry;
    }

    return ssl_want::want_nothing;
}

ssl_want SSLHandshakeManager::doClientHandshake(asio::error_code& ec,
                                                HandshakeState* pHandshakeState) {
    DWORD sspiFlags = getClientFlags() | ISC_REQ_ALLOCATE_MEMORY;

    std::array<SecBuffer, 3> outputBuffers;

    outputBuffers[0].cbBuffer = 0;
    outputBuffers[0].BufferType = SECBUFFER_TOKEN;
    outputBuffers[0].pvBuffer = NULL;
    ContextBufferDeleter deleter(&outputBuffers[0].pvBuffer);

    outputBuffers[1].cbBuffer = 0;
    outputBuffers[1].BufferType = SECBUFFER_ALERT;
    outputBuffers[1].pvBuffer = NULL;
    ContextBufferDeleter alertDeleter(&outputBuffers[1].pvBuffer);

    outputBuffers[2].cbBuffer = 0;
    outputBuffers[2].BufferType = SECBUFFER_EMPTY;
    outputBuffers[2].pvBuffer = NULL;

    SecBufferDesc outputBufferDesc;
    outputBufferDesc.ulVersion = SECBUFFER_VERSION;
    outputBufferDesc.cBuffers = outputBuffers.size();
    outputBufferDesc.pBuffers = outputBuffers.data();

    std::array<SecBuffer, 2> inputBuffers;

    SECURITY_STATUS ss;
    TimeStamp lifetime;
    ULONG retAttribs;

    // If the input buffer is empty, this is the start of the client handshake.
    if (!_pInBuffer->empty()) {
        inputBuffers[0].cbBuffer = _pInBuffer->size();
        inputBuffers[0].BufferType = SECBUFFER_TOKEN;
        inputBuffers[0].pvBuffer = _pInBuffer->data();

        inputBuffers[1].cbBuffer = 0;
        inputBuffers[1].BufferType = SECBUFFER_EMPTY;
        inputBuffers[1].pvBuffer = NULL;

        SecBufferDesc inputBufferDesc;
        inputBufferDesc.ulVersion = SECBUFFER_VERSION;
        inputBufferDesc.cBuffers = inputBuffers.size();
        inputBufferDesc.pBuffers = inputBuffers.data();

        ss = InitializeSecurityContextW(_phcred,
                                        _phctxt,
                                        const_cast<SEC_WCHAR*>(_serverName.c_str()),
                                        sspiFlags,
                                        0,
                                        0,
                                        &inputBufferDesc,
                                        0,
                                        _phctxt,
                                        &outputBufferDesc,
                                        &retAttribs,
                                        &lifetime);
    } else {
        ss = InitializeSecurityContextW(_phcred,
                                        NULL,
                                        const_cast<SEC_WCHAR*>(_serverName.c_str()),
                                        sspiFlags,
                                        0,
                                        0,
                                        NULL,
                                        0,
                                        _phctxt,
                                        &outputBufferDesc,
                                        &retAttribs,
                                        &lifetime);
    }

    if (ss < SEC_E_OK) {
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            return ssl_want::want_input_and_retry;
        }

        ec = asio::error_code(ss, asio::error::get_ssl_category());

        if ((retAttribs & ISC_RET_EXTENDED_ERROR) && (outputBuffers[1].cbBuffer > 0)) {
            _pOutBuffer->reset();
            _pOutBuffer->append(outputBuffers[0].pvBuffer, outputBuffers[0].cbBuffer);

            // Tell ASIO we have something to send back the last data
            return ssl_want::want_output;
        }

        return ssl_want::want_nothing;
    }
    // ASC_RET_EXTENDED_ERROR is not support on Windows 7/Windows 2008 R2
    invariant(sspiFlags == (retAttribs | ASC_RET_EXTENDED_ERROR));

    LOGV2_DEBUG(7998002,
                2,
                "TLS client ISC result",
                "ss"_attr = ss,
                "outputBytes"_attr = outputBuffers[0].cbBuffer,
                "inputSize"_attr = _pInBuffer->size());

    if (_pInBuffer->size()) {
        // Locate (optional) extra buffer
        if (inputBuffers[1].BufferType == SECBUFFER_EXTRA && inputBuffers[1].cbBuffer > 0) {
            // SECBUFFER_EXTRA do not set pvBuffer, just cbBuffer
            // cbBuffer tells us how much remaining in the buffer is extra
            _pExtraEncryptedBuffer->reset();
            _pExtraEncryptedBuffer->append(_pInBuffer->data() +
                                               (_pInBuffer->size() - inputBuffers[1].cbBuffer),
                                           inputBuffers[1].cbBuffer);
        }
    }

    // Next, figure out if we need to send any data out
    bool needOutput{false};

    // ISC says to continue, or it finished (SEC_E_OK) but still produced token bytes to send.
    if (SEC_I_CONTINUE_NEEDED == ss || SEC_I_COMPLETE_AND_CONTINUE == ss ||
        (SEC_E_OK == ss && outputBuffers[0].cbBuffer != 0)) {
        needOutput = true;
    }

    _pOutBuffer->reset();
    _pOutBuffer->append(outputBuffers[0].pvBuffer, outputBuffers[0].cbBuffer);

    // Reset the input buffer
    _pInBuffer->reset();

    // Check if we have any additional encrypted data.
    // When SEC_E_OK, the handshake is done and any leftover bytes belong to the read path
    // (e.g. a TLS 1.3 NewSessionTicket already received inline with the server flight).
    // Do NOT call setState(NeedMoreHandshakeData) in that case — it would conflict with the
    // Done signal below and cause an assertion failure in the state-machine transitions.
    if (!_pExtraEncryptedBuffer->empty()) {
        _pInBuffer->swap(*_pExtraEncryptedBuffer);
        _pExtraEncryptedBuffer->reset();

        if (SEC_E_OK != ss) {
            // When doing the handshake and we have extra data, this means we have an incomplete
            // TLS record and need more bytes to complete it.
            setState(State::NeedMoreHandshakeData);
        }
    }

    if (needOutput) {
        // TLS 1.3: InitializeSecurityContextW may return SEC_E_OK with non-zero output when
        // the client's final flight (Certificate + CertificateVerify + Finished) is ready.
        // The handshake is complete on the client side; signal Done so the _handshake loop
        // sends this data and exits.  Returning want_output_and_retry here would cause the
        // loop to re-enter want_input_and_retry and block forever waiting for more server
        // handshake data, while the server has already transitioned to application-data mode.
        if (SEC_E_OK == ss) {
            LOGV2_DEBUG(7998003,
                        2,
                        "TLS client handshake done with pending output (TLS 1.3)",
                        "outputBytes"_attr = outputBuffers[0].cbBuffer);
            *pHandshakeState = HandshakeState::Done;
            return ssl_want::want_output;
        }
        return ssl_want::want_output_and_retry;
    }

    return ssl_want::want_nothing;
}

/**
 * Read decrypted data if encrypted data was provided via writeData and succesfully decrypted.
 */
ssl_want SSLReadManager::readDecryptedData(void* data,
                                           std::size_t length,
                                           asio::error_code& ec,
                                           std::size_t& bytes_transferred,
                                           DecryptState* pDecryptState) {
    bytes_transferred = 0;
    ec = asio::error_code();
    *pDecryptState = DecryptState::Continue;

    // Our last state was that we needed more encrypted data, so tell ASIO we still want some
    if (_state == State::NeedMoreEncryptedData) {
        return ssl_want::want_input_and_retry;
    }

    // If we have encrypted data, try to decrypt it
    if (_state == State::HaveEncryptedData) {
        ssl_want wantState = decryptBuffer(ec, pDecryptState);
        if (ec) {
            return wantState;
        }

        // If remote side started shutdown, bail
        if (*pDecryptState != DecryptState::Continue) {
            return ssl_want::want_nothing;
        }

        if (wantState == ssl_want::want_input_and_retry) {
            setState(State::NeedMoreEncryptedData);
        }

        if (wantState != ssl_want::want_nothing) {
            return wantState;
        }
    }

    // We decrypted data in the past, hand it back to ASIO until we are out of decrypted data
    ASIO_ASSERT(_state == State::HaveDecryptedData);

    _pInBuffer->readInto(data, length, bytes_transferred);

    // Have we read all the decrypted data?
    if (_pInBuffer->empty()) {
        // If we have some extra encrypted data, it needs to be checked if it is at least a
        // valid SSL packet, so set the state machine to reflect that we have some encrypted
        // data.
        if (!_pExtraEncryptedBuffer->empty()) {
            _pInBuffer->swap(*_pExtraEncryptedBuffer);
            _pExtraEncryptedBuffer->reset();
            setState(State::HaveEncryptedData);
        } else {
            // We are empty so reset our state to need encrypted data for the next call
            setState(State::NeedMoreEncryptedData);
        }
    }

    return ssl_want::want_nothing;
}

ssl_want SSLReadManager::decryptBuffer(asio::error_code& ec, DecryptState* pDecryptState) {
    while (true) {
        // Save original buffer pointer and size before DecryptMessage.  For status 0x80090317,
        // Schannel sets securityBuffers[0].cbBuffer to the consumed NST record size rather than
        // the full input size, which would cause the TLS-header fallback to miss trailing bytes
        // (e.g. a KMIP response that arrived in the same recv() call as the NewSessionTicket).
        const auto* const preCallInputPtr = static_cast<const uint8_t*>(_pInBuffer->data());
        const ULONG preCallInputLen = static_cast<ULONG>(_pInBuffer->size());

        LOGV2_DEBUG(7998035,
                    0,
                    "TLS decryptBuffer: calling DecryptMessage",
                    "inputBytes"_attr = preCallInputLen);

        std::array<SecBuffer, 4> securityBuffers;
        securityBuffers[0].cbBuffer = preCallInputLen;
        securityBuffers[0].BufferType = SECBUFFER_DATA;
        securityBuffers[0].pvBuffer = _pInBuffer->data();

        securityBuffers[1].cbBuffer = 0;
        securityBuffers[1].BufferType = SECBUFFER_EMPTY;
        securityBuffers[1].pvBuffer = NULL;

        securityBuffers[2].cbBuffer = 0;
        securityBuffers[2].BufferType = SECBUFFER_EMPTY;
        securityBuffers[2].pvBuffer = NULL;

        securityBuffers[3].cbBuffer = 0;
        securityBuffers[3].BufferType = SECBUFFER_EMPTY;
        securityBuffers[3].pvBuffer = NULL;

        SecBufferDesc bufferDesc;
        bufferDesc.ulVersion = SECBUFFER_VERSION;
        bufferDesc.cBuffers = securityBuffers.size();
        bufferDesc.pBuffers = securityBuffers.data();

        SECURITY_STATUS ss = DecryptMessage(_phctxt, &bufferDesc, 0, NULL);

        LOGV2_DEBUG(7998028,
                    1,
                    "TLS DecryptMessage result",
                    "status"_attr = static_cast<int32_t>(ss),
                    "inputBytes"_attr = securityBuffers[0].cbBuffer);

        if (ss < SEC_E_OK) {
            if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                return ssl_want::want_input_and_retry;
            } else if (ss == static_cast<SECURITY_STATUS>(0x80090317L)) {
                // 0x80090317 is the error-severity form of SEC_I_CONTEXT_EXPIRED.  Schannel
                // returns it from DecryptMessage after consuming a TLS 1.3 post-handshake
                // message (e.g. NewSessionTicket) from an OpenSSL peer.  Treat it like
                // SEC_I_RENEGOTIATE on TLS 1.3: the record is already consumed internally;
                // preserve any trailing bytes for the next decryption call.
                //
                // Schannel may not populate SECBUFFER_EXTRA for this status code even when the
                // input buffer contains bytes beyond the consumed TLS record (e.g. a KMIP
                // response that arrived in the same recv() call).  As a fallback, parse the
                // 5-byte TLS record header (ContentType + Version[2] + Length[2]) to locate
                // the boundary and rescue any trailing bytes before resetting _pInBuffer.
                //
                // Use the pre-call pointer and size (not securityBuffers[0] post-call values)
                // because DecryptMessage sets securityBuffers[0].cbBuffer to the consumed NST
                // record size, which causes recordSize == inputLen and extraBytes = 0, losing
                // any trailing bytes (e.g. a KMIP response in the same TCP segment).
                const auto* inputPtr = preCallInputPtr;
                const ULONG inputLen = preCallInputLen;

                ULONG extraBytes = 0;
                const uint8_t* extraPtr = nullptr;

                if (securityBuffers[3].BufferType == SECBUFFER_EXTRA &&
                    securityBuffers[3].pvBuffer != nullptr && securityBuffers[3].cbBuffer > 0) {
                    extraBytes = securityBuffers[3].cbBuffer;
                    extraPtr = static_cast<const uint8_t*>(securityBuffers[3].pvBuffer);
                } else if (inputLen >= 5) {
                    // TLS record header: bytes [3..4] are the payload length (big-endian).
                    const uint32_t recordSize =
                        5u + ((static_cast<uint32_t>(inputPtr[3]) << 8) | inputPtr[4]);
                    if (recordSize < inputLen) {
                        extraBytes = inputLen - recordSize;
                        extraPtr = inputPtr + recordSize;
                    }
                }

                LOGV2_DEBUG(7998029,
                            0,
                            "TLS 1.3 post-handshake message (0x80090317) received from OpenSSL "
                            "peer: consuming NewSessionTicket and preserving trailing bytes",
                            "extraBytes"_attr = extraBytes,
                            "usedTLSHeaderFallback"_attr =
                                (extraBytes > 0 && securityBuffers[3].cbBuffer == 0),
                            "postCallCbBuffer"_attr = securityBuffers[0].cbBuffer,
                            "preCallInputLen"_attr = preCallInputLen);

                if (extraBytes > 0) {
                    ASIO_ASSERT(_pExtraEncryptedBuffer->empty());
                    _pExtraEncryptedBuffer->append(extraPtr, extraBytes);
                }
                _pInBuffer->reset();
                if (!_pExtraEncryptedBuffer->empty()) {
                    _pInBuffer->swap(*_pExtraEncryptedBuffer);
                    _pExtraEncryptedBuffer->reset();
                    continue;
                }

                return ssl_want::want_input_and_retry;
            } else {
                LOGV2_DEBUG(7998027,
                            0,
                            "TLS DecryptMessage failed",
                            "status"_attr = ss,
                            "inputBytes"_attr = securityBuffers[0].cbBuffer);
                ec = asio::error_code(ss, asio::error::get_ssl_category());
                return ssl_want::want_nothing;
            }
        }

        // Shutdown has been initiated at the client side
        if (ss == SEC_I_CONTEXT_EXPIRED) {
            LOGV2_DEBUG(7998026,
                        2,
                        "TLS DecryptMessage: received close_notify or context expired "
                        "(SEC_I_CONTEXT_EXPIRED)",
                        "inputBytes"_attr = securityBuffers[0].cbBuffer);
            *pDecryptState = DecryptState::Shutdown;
        } else if (ss == SEC_I_RENEGOTIATE) {
            // Schannel returns SEC_I_RENEGOTIATE from DecryptMessage for two distinct cases:
            //
            //  TLS 1.2: The peer sent a HelloRequest (server-initiated renegotiation).
            //           MongoDB blocks renegotiation — fail the connection as before.
            //
            //  TLS 1.3: A post-handshake message was received: NewSessionTicket (the server
            //           is distributing session-resumption material) or KeyUpdate (traffic-key
            //           rotation).  These are *not* renegotiation; TLS 1.3 removed that
            //           feature entirely.  Schannel reuses SEC_I_RENEGOTIATE for both cases.
            //           Schannel processes the message internally; no ISC/ASC call is needed
            //           (calling ISC/ASC here corrupts the application traffic keys).
            //
            // Determine the negotiated protocol to pick the right behaviour.
            SecPkgContext_ConnectionInfo connInfo{};
            SECURITY_STATUS qi =
                QueryContextAttributesW(_phctxt, SECPKG_ATTR_CONNECTION_INFO, &connInfo);
            const bool isTLS13 = (qi == SEC_E_OK) && ((connInfo.dwProtocol & SP_PROT_TLS1_3) != 0);

            if (!isTLS13) {
                // TLS 1.2 renegotiation: block it.
                *pDecryptState = DecryptState::Renegotiate;
                ec = asio::ssl::error::no_renegotiation;
                return ssl_want::want_nothing;
            }

            // Do NOT call ISC/ASC here.
            //
            // Calling InitializeSecurityContext or AcceptSecurityContext after DecryptMessage
            // returns SEC_I_RENEGOTIATE for a TLS 1.3 NewSessionTicket or KeyUpdate — even
            // with an existing phContext and an empty/NULL input descriptor — causes Schannel
            // to corrupt its application-layer traffic keys.  The next DecryptMessage call
            // then fails with SEC_E_DECRYPT_FAILURE (0x80090330), "The specified data could
            // not be decrypted."
            //
            // Schannel processes the post-handshake message internally when DecryptMessage
            // returns SEC_I_RENEGOTIATE.  No ISC/ASC call is required.  Any trailing bytes
            // in the input buffer (SECBUFFER_EXTRA) are the start of the next TLS record and
            // must be preserved for the next decryption.

            ULONG extraBytes =
                (securityBuffers[3].BufferType == SECBUFFER_EXTRA &&
                 securityBuffers[3].pvBuffer != nullptr && securityBuffers[3].cbBuffer > 0)
                ? securityBuffers[3].cbBuffer
                : 0;
            LOGV2_DEBUG(7998005,
                        2,
                        "TLS 1.3 post-handshake message processed by Schannel (no ISC/ASC needed)",
                        "extraBytes"_attr = extraBytes);

            // Save any extra encrypted data that arrived after the post-handshake record
            // before we reset _pInBuffer (SECBUFFER_EXTRA pvBuffer points into _pInBuffer).
            if (extraBytes > 0) {
                ASIO_ASSERT(_pExtraEncryptedBuffer->empty());
                _pExtraEncryptedBuffer->append(securityBuffers[3].pvBuffer,
                                               securityBuffers[3].cbBuffer);
            }
            _pInBuffer->reset();  // The post-handshake record has been consumed.

            LOGV2_DEBUG(7998015,
                        2,
                        "TLS 1.3 post-handshake: continuing read after NewSessionTicket/KeyUpdate",
                        "extraBufferEmpty"_attr = _pExtraEncryptedBuffer->empty());

            // Continue reading: process extra data if available, else request more.
            if (!_pExtraEncryptedBuffer->empty()) {
                _pInBuffer->swap(*_pExtraEncryptedBuffer);
                _pExtraEncryptedBuffer->reset();
                continue;
            }
            return ssl_want::want_input_and_retry;
        }

        // The network layer may have read more then 1 SSL packet so remember the extra data.
        if (securityBuffers[3].BufferType == SECBUFFER_EXTRA &&
            securityBuffers[3].pvBuffer != nullptr && securityBuffers[3].cbBuffer > 0) {
            ASIO_ASSERT(_pExtraEncryptedBuffer->empty());
            _pExtraEncryptedBuffer->append(securityBuffers[3].pvBuffer,
                                           securityBuffers[3].cbBuffer);
        }

        // Check if we have application data
        if (securityBuffers[1].cbBuffer > 0) {
            _pInBuffer->resetPos(securityBuffers[1].pvBuffer, securityBuffers[1].cbBuffer);

            setState(State::HaveDecryptedData);

            return ssl_want::want_nothing;
        } else {
            // Clear the existing TLS packet from the input buffer since it was completely empty
            // and we have already processed any extra data.
            _pInBuffer->reset();

            // Sigh, this means that the remote side sent us an TLS record with just a encryption
            // header/trailer but no actual data.
            //
            // If we have extra encrypted data, we may have a TLS record with some data, otherwise
            // we need more data from the remote side
            if (!_pExtraEncryptedBuffer->empty()) {
                _pInBuffer->swap(*_pExtraEncryptedBuffer);
                _pExtraEncryptedBuffer->reset();
                continue;
            }

            return ssl_want::want_input_and_retry;
        }
    }
}


/**
 * Encrypts data to be sent to the remote side.
 *
 * If the message is >= max packet size, it will return want_output_and_retry, and expects
 * callers to continue to call it with the same parameters until want_output is returned.
 */
ssl_want SSLWriteManager::writeUnencryptedData(const void* pMessage,
                                               std::size_t messageLength,
                                               std::size_t& bytes_transferred,
                                               asio::error_code& ec) {
    ec = asio::error_code();

    if (_securityTrailerLength == ULONG_MAX) {
        SecPkgContext_StreamSizes secPkgContextStreamSizes;

        SECURITY_STATUS ss =
            QueryContextAttributes(_phctxt, SECPKG_ATTR_STREAM_SIZES, &secPkgContextStreamSizes);

        if (ss < SEC_E_OK) {
            ec = asio::error_code(ss, asio::error::get_ssl_category());
            return ssl_want::want_nothing;
        }

        _securityTrailerLength = secPkgContextStreamSizes.cbTrailer;
        _securityMaxMessageLength = secPkgContextStreamSizes.cbMaximumMessage;
        _securityHeaderLength = secPkgContextStreamSizes.cbHeader;
    }

    // Do we need to fragment the message out?
    if (messageLength > _securityMaxMessageLength) {
        // Since the message is too large for SSL, we have to write out fragments. We rely on
        // the fact that ASIO will keep giving us the same buffer back as long as it is asked to
        // retry.
        std::size_t fragmentLength =
            std::min(_securityMaxMessageLength, messageLength - _lastWriteOffset);
        ssl_want want = encryptMessage(reinterpret_cast<const char*>(pMessage) + _lastWriteOffset,
                                       fragmentLength,
                                       bytes_transferred,
                                       ec);
        if (ec) {
            return want;
        }

        _lastWriteOffset += fragmentLength;

        // We have more data to give ASIO after this fragment
        if (_lastWriteOffset < messageLength) {
            return ssl_want::want_output_and_retry;
        }

        // We have consumed all the data given to us over multiple consecutive calls, reset
        // position.
        _lastWriteOffset = 0;

        // ASIO's buffering of engine calls assumes that bytes_transfered refers to all the
        // bytes we transfered total when want_output is returned. It ignores bytes_transfered
        // when want_output_and_retry is returned;
        bytes_transferred = messageLength;

        return ssl_want::want_output;
    } else {
        // Reset fragmentation position
        _lastWriteOffset = 0;

        // Send message as is without fragmentation
        return encryptMessage(pMessage, messageLength, bytes_transferred, ec);
    }
}

ssl_want SSLWriteManager::encryptMessage(const void* pMessage,
                                         std::size_t messageLength,
                                         std::size_t& bytes_transferred,
                                         asio::error_code& ec) {
    ASIO_ASSERT(_pOutBuffer->empty());
    _pOutBuffer->resize(_securityTrailerLength + _securityHeaderLength + messageLength);

    std::array<SecBuffer, 4> securityBuffers;

    securityBuffers[0].BufferType = SECBUFFER_STREAM_HEADER;
    securityBuffers[0].cbBuffer = _securityHeaderLength;
    securityBuffers[0].pvBuffer = _pOutBuffer->data();

    memcpy_s(_pOutBuffer->data() + _securityHeaderLength,
             _pOutBuffer->size() - _securityHeaderLength - _securityTrailerLength,
             pMessage,
             messageLength);

    securityBuffers[1].BufferType = SECBUFFER_DATA;
    securityBuffers[1].cbBuffer = messageLength;
    securityBuffers[1].pvBuffer = _pOutBuffer->data() + _securityHeaderLength;

    securityBuffers[2].cbBuffer = _securityTrailerLength;
    securityBuffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
    securityBuffers[2].pvBuffer = _pOutBuffer->data() + _securityHeaderLength + messageLength;

    securityBuffers[3].cbBuffer = 0;
    securityBuffers[3].BufferType = SECBUFFER_EMPTY;
    securityBuffers[3].pvBuffer = 0;

    SecBufferDesc bufferDesc;

    bufferDesc.ulVersion = SECBUFFER_VERSION;
    bufferDesc.cBuffers = securityBuffers.size();
    bufferDesc.pBuffers = securityBuffers.data();

    SECURITY_STATUS ss = EncryptMessage(_phctxt, 0, &bufferDesc, 0);

    if (ss < SEC_E_OK) {
        LOGV2_DEBUG(7998030,
                    1,
                    "TLS EncryptMessage failed",
                    "status"_attr = static_cast<int32_t>(ss),
                    "messageLength"_attr = messageLength);
        ec = asio::error_code(ss, asio::error::get_ssl_category());
        return ssl_want::want_nothing;
    }

    size_t size =
        securityBuffers[0].cbBuffer + securityBuffers[1].cbBuffer + securityBuffers[2].cbBuffer;

    _pOutBuffer->resize(size);

    // Tell asio that all the clear text was transfered.
    bytes_transferred = messageLength;

    return ssl_want::want_output;
}


MONGO_INITIALIZER(InitializeSchannelGetServerIdentityFn)(mongo::InitializerContext*) {
    auto sc = std::move(mongo::SharedLibrary::create("Schannel.dll").getValue());

    SSLHandshakeManager::setSslGetServerIdentityFn(
        sc->getFunctionAs<SslGetServerIdentityFn>("SslGetServerIdentity").getValue());
}

}  // namespace detail
}  // namespace ssl
}  // namespace asio
