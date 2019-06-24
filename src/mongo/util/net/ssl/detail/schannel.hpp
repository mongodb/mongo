
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

#include "asio/detail/config.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>

#include "asio/detail/push_options.hpp"


#include "asio/detail/assert.hpp"

#define ASSERT_STATE_TRANSITION(orig, dest) ASIO_ASSERT(!(orig) || (dest));

namespace asio {
namespace ssl {
namespace detail {

/**
 * Reusable buffer. Behaves as a sort of producer consumer queue in a sense.
 *
 * Data is added to the buffer then removed.
 *
 * Typical workflow:
 * - Write data
 * - Write more data
 * - Read some data
 * - Keeping reading until empty
 *
 * Invariants:
 * - Once reading from a buffer is started, no more writes are permitted until
 *   consumer has read all the entire buffer.
 */
class ReusableBuffer {
public:
    ReusableBuffer(std::size_t initialSize) {
        _buffer = std::make_unique<std::uint8_t[]>(initialSize);
        _capacity = initialSize;
    }

    /**
     * Is buffer empty?
     */
    bool empty() const {
        return _size == 0;
    }

    /**
     * Get raw pointer to buffer.
     */
    std::uint8_t* data() {
        return _buffer.get();
    }

    /**
     * Get current number of elements in buffer.
     */
    std::size_t size() const {
        return _size;
    }

    /**
     * Reset to empty state.
     */
    void reset() {
        _bufPos = 0;
        _size = 0;
    }

    /**
     * Add data to empty buffer.
     */
    void fill(const std::vector<std::uint8_t>& vec) {
        ASIO_ASSERT(_size == 0);
        ASIO_ASSERT(_bufPos == 0);
        append(vec.data(), vec.size());
    }

    /**
     * Reset current position to specified pointer in buffer.
     */
    void resetPos(void* pos, std::size_t size) {
        ASIO_ASSERT(pos >= _buffer.get() && pos < (_buffer.get() + _size));
        _bufPos = (std::uint8_t*)pos - _buffer.get();
        resize(_bufPos + size);
    }

    /**
     * Append data to buffer.
     */
    void append(const void* data, std::size_t length) {
        ASIO_ASSERT(_bufPos == 0);
        auto originalSize = _size;
        resize(_size + length);
        std::copy(reinterpret_cast<const std::uint8_t*>(data),
                  reinterpret_cast<const std::uint8_t*>(data) + length,
                  _buffer.get() + originalSize);
    }

    /**
     * Read data from buffer. Can be a partial read.
     */
    void readInto(void* data, std::size_t length, std::size_t& outLength) {
        if (length >= (size() - _bufPos)) {
            // We have less then ASIO wants, give them everything we have
            outLength = size() - _bufPos;
            memcpy_s(data, length, _buffer.get() + _bufPos, size() - _bufPos);

            // We are empty so reset our state to need encrypted data for the next call
            _bufPos = 0;
            _size = 0;
        } else {
            // ASIO wants less then we have so give them just what they want
            outLength = length;
            memcpy_s(data, length, _buffer.get() + _bufPos, length);

            _bufPos += length;
        }
    }

    /**
     * Realloc buffer preserving existing data.
     */
    void resize(std::size_t size) {
        if (size > _capacity) {
            auto temp = std::make_unique<unsigned char[]>(size);

            memcpy_s(temp.get(), size, _buffer.get(), _size);
            _buffer.swap(temp);
            _capacity = size;
        }
        _size = size;
    }

    /**
     * Swap current buffer with other buffer.
     */
    void swap(ReusableBuffer& other) {
        std::swap(_buffer, other._buffer);
        std::swap(_bufPos, other._bufPos);
        std::swap(_size, other._size);
        std::swap(_capacity, other._capacity);
    }

private:
    // Buffer of data
    std::unique_ptr<std::uint8_t[]> _buffer;

    // Current read position in buffer
    std::size_t _bufPos{0};

    // Count of elements in buffer
    std::size_t _size{0};

    // Capacity of buffer for elements, always >= _size
    std::size_t _capacity;
};

// Default buffer size. SSL has a max encapsulated packet size of 16 kb.
const std::size_t kDefaultBufferSize = 17 * 1024;

// This enum mirrors the engine::want enum. The values must be kept in sync
// to support a simple conversion from ssl_want to engine::want, see ssl_want_to_engine.
enum class ssl_want {
    // Returned by functions to indicate that the engine wants input. The input
    // buffer should be updated to point to the data. The engine then needs to
    // be called again to retry the operation.
    want_input_and_retry = -2,

    // Returned by functions to indicate that the engine wants to write output.
    // The output buffer points to the data to be written. The engine then
    // needs to be called again to retry the operation.
    want_output_and_retry = -1,

    // Returned by functions to indicate that the engine doesn't need input or
    // output.
    want_nothing = 0,

    // Returned by functions to indicate that the engine wants to write output.
    // The output buffer points to the data to be written. After that the
    // operation is complete, and the engine does not need to be called again.
    want_output = 1
};

/**
 * Manages the SSL handshake and shutdown state machines.
 *
 * Handshakes are always the first set of events during SSL connection initiation.
 * Shutdown can occur anytime after the handshake has succesfully finished
 * as a result of a read event or explicit shutdown request from the engine.
 */
class SSLHandshakeManager {
public:
    /**
     * Handshake Mode indicates whether this a for a client or server side.
     *
     * Each given connection can only be a client or server, and it cannot change once set.
     */
    enum class HandshakeMode {
        // Initial state, illegal for clients to set
        Unknown,

        // Client handshake, connect side
        Client,

        // Server handshake, accept side
        Server,
    };

    /**
     * Handshake state indicates to the caller if nextHandshake needs to be called next.
     */
    enum class HandshakeState {
        // Caller should continue to call nextHandshake, the handshake is not done.
        Continue,

        // Caller should not continue to call nextHandshake, the handshake is done.
        Done
    };

    SSLHandshakeManager(PCtxtHandle hctxt,
                        PCredHandle phcred,
                        std::wstring& serverName,
                        ReusableBuffer* pInBuffer,
                        ReusableBuffer* pOutBuffer,
                        ReusableBuffer* pExtraBuffer,
                        SCHANNEL_CRED* cred)
        : _state(State::HandshakeStart),
          _phctxt(hctxt),
          _cred(cred),
          _serverName(serverName),
          _phcred(phcred),
          _pInBuffer(pInBuffer),
          _pOutBuffer(pOutBuffer),
          _pExtraEncryptedBuffer(pExtraBuffer),
          _alertBuffer(1024),
          _mode(HandshakeMode::Unknown) {}

    /**
     * Set the current handdshake mode as client or server.
     *
     * Idempotent if called with same mode otherwise it asserts.
     */
    void setMode(HandshakeMode mode) {
        ASIO_ASSERT(_mode == HandshakeMode::Unknown || _mode == mode);
        ASIO_ASSERT(mode != HandshakeMode::Unknown);
        _mode = mode;
    }

    /**
     * Start or continue SSL handshake.
     *
     * Must be called until HandshakeState::Done is returned.
     *
     * Return status code to indicate whether it needs more data or if data needs to be sent to the
     * other side.
     */
    ssl_want nextHandshake(asio::error_code& ec, HandshakeState* pHandshakeState);

    /**
     * Begin graceful SSL shutdown. Either:
     * - respond to already received alert signalling connection shutdown on remote side
     * - start SSL shutdown by signalling remote side
     */
    ssl_want beginShutdown(asio::error_code& ec);

    /*
     * Ingest data from ASIO that has been received.
     */
    void writeEncryptedData(const void* data, std::size_t length);

    /**
     * Returns true if there is data to send over the wire.
     */
    bool hasOutputData() {
        return !_pOutBuffer->empty();
    }

    /**
     * Get data to sent over the network.
     */
    void readOutputBuffer(void* data, size_t inLength, size_t& outLength) {
        _pOutBuffer->readInto(data, inLength, outLength);
    }

private:
    void startServerHandshake(asio::error_code& ec);

    void startClientHandshake(asio::error_code& ec);

    DWORD getServerFlags() {
        return ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
            ASC_REQ_EXTENDED_ERROR | ASC_REQ_STREAM | ASC_REQ_MUTUAL_AUTH;
    }

    DWORD getClientFlags() {
        return ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
            ISC_REQ_EXTENDED_ERROR | ISC_REQ_STREAM | ISC_REQ_USE_SUPPLIED_CREDS |
            ISC_REQ_MANUAL_CRED_VALIDATION;
    }

    /**
     * RAII class to free a buffer allocated by SSPI.
     */
    class ContextBufferDeleter {
    public:
        ContextBufferDeleter(void** buf) : _buf(buf) {}

        ~ContextBufferDeleter() {
            if (*_buf != nullptr) {
                FreeContextBuffer(*_buf);
            }
        }

    private:
        void** _buf;
    };

    ssl_want startShutdown(asio::error_code& ec);

    ssl_want doServerHandshake(asio::error_code& ec, HandshakeState* pHandshakeState);

    ssl_want doClientHandshake(asio::error_code& ec);

private:
    /**
     * Handshake State machine:
     *                          +-----------------------------+
     *                          v                             |
     * +----------------+     +-----------------------+     +-------------------+     +------+
     * | HandshakeStart | --> | NeedMoreHandshakeData | --> | HaveEncryptedData | --> | Done |
     * +----------------+     +-----------------------+     +-------------------+     +------+
     *                          ^                   |
     *                          +-------------------+
     *
     * echo "[ HandshakeStart ] --> [ NeedMoreHandshakeData ] --> [ NeedMoreHandshakeData ] -->
     * [HaveEncryptedData] -> [NeedMoreHandshakeData], [Done]" | graph-easy "[ HandshakeStart ] -->
     * [ NeedMoreHandshakeData ] --> [HaveEncryptedData] -> [
     */
    enum class State {
        // Initial state
        HandshakeStart,

        // Handshake needs more data before it decode the next message
        NeedMoreHandshakeData,

        // Handshake just received some data, and can now try to decrypt it
        HaveEncryptedData,

        // Handshake is done
        Done,
    };

    /**
     * Transition state machine
     */
    void setState(State s) {
        ASSERT_STATE_TRANSITION(_state == State::HandshakeStart, s == State::NeedMoreHandshakeData);
        ASSERT_STATE_TRANSITION(_state == State::NeedMoreHandshakeData,
                                s == State::HaveEncryptedData || s == State::NeedMoreHandshakeData);
        ASSERT_STATE_TRANSITION(_state == State::HaveEncryptedData,
                                s == State::NeedMoreHandshakeData || s == State::Done);
        _state = s;
    }

private:
    // State machine
    State _state;

    // Handshake mode - client or server
    HandshakeMode _mode;

    // Server name for TLS SNI purposes
    std::wstring& _serverName;

    // Buffer of data received from remote side
    ReusableBuffer* _pInBuffer;

    // Scratch buffer to capture extra handshake data
    ReusableBuffer* _pExtraEncryptedBuffer;

    // Buffer to data to send to remote side
    ReusableBuffer* _pOutBuffer;

    // Buffer of data received from remote side
    ReusableBuffer _alertBuffer;

    // SChannel Credentials
    SCHANNEL_CRED* _cred;

    // SChannel context
    PCtxtHandle _phctxt;

    // Credential handle
    PCredHandle _phcred;
};

/**
 * Manages the SSL read state machine.
 *
 * Notifies callers of graceful SSL shutdown events.
 */
class SSLReadManager {
public:
    /**
     * Indicates whether client should continue to decrypt data or it needs to handle other protocol
     * signals.
     */
    enum class DecryptState {
        // SSL connection is proceeding normally
        Continue,

        // Remote side has signaled graceful SSL shutdown
        Shutdown,

        // Remote side has signaled renegtiation
        Renegotiate,
    };

    SSLReadManager(PCtxtHandle hctxt,
                   PCredHandle hcred,
                   ReusableBuffer* pInBuffer,
                   ReusableBuffer* pExtraBuffer)
        : _state(State::NeedMoreEncryptedData),
          _phctxt(hctxt),
          _phcred(hcred),
          _pInBuffer(pInBuffer),
          _pExtraEncryptedBuffer(pExtraBuffer) {}

    /**
     * Read decrypted data if encrypted data was provided via writeData and succesfully decrypted.
     */
    ssl_want readDecryptedData(void* data,
                               std::size_t length,
                               asio::error_code& ec,
                               std::size_t& bytes_transferred,
                               DecryptState* pDecryptState);

    /**
     * Receive more data from ASIO.
     */
    void writeData(const void* data, std::size_t length) {
        ASIO_ASSERT(_pExtraEncryptedBuffer->empty());

        // We have more data, it may not be enough to decode but we will figure that out later.
        setState(State::HaveEncryptedData);

        _pInBuffer->append(data, length);
    }

private:
    ssl_want decryptBuffer(asio::error_code& ec, DecryptState* pDecryptState);

private:
    /**
     * Read State machine:
     *
     *  +------------------------------------------------------------+
     *  |                                                            |
     *  |                                                            |
     *  |    +-----------------------------+                         |
     *  |    v                             |                         |
     *  |  +-----------------------+     +-------------------+     +-------------------+
     *  +> | NeedMoreEncryptedData | --> | HaveEncryptedData | --> | HaveDecryptedData |
     *     +-----------------------+     +-------------------+     +-------------------+
     *       ^                   |         ^                         |
     *       +-------------------+         +-------------------------+
     *
     * "[ NeedMoreEncryptedData ] --> [ HaveEncryptedData ] --> [HaveDecryptedData] ->
     * [NeedMoreEncryptedData], [HaveEncryptedData] --> [NeedMoreEncryptedData]  " | graph-easy
     *
     */
    enum class State {
        // Initial state, Need more data from remote side
        NeedMoreEncryptedData,

        // Have some encrypted data, unknown if it is a complete packet
        HaveEncryptedData,

        // Was able to decrypt a packet, give decrypted data back to client
        HaveDecryptedData,
    };

    /**
     * Transition state machine
     */
    void setState(State s) {
        ASSERT_STATE_TRANSITION(_state == State::NeedMoreEncryptedData,
                                s == State::HaveEncryptedData);
        ASSERT_STATE_TRANSITION(
            _state == State::HaveEncryptedData,
            (s == State::NeedMoreEncryptedData || s == State::HaveDecryptedData));
        ASSERT_STATE_TRANSITION(
            _state == State::HaveDecryptedData,
            (s == State::NeedMoreEncryptedData || s == State::HaveEncryptedData));
        _state = s;
    }

private:
    // State machine
    State _state;

    // Scratch buffer to capture extra decryption data
    ReusableBuffer* _pExtraEncryptedBuffer;

    // Buffer of data from the remote side
    ReusableBuffer* _pInBuffer;

    // SChannel context handle
    PCtxtHandle _phctxt;

    // Credential handle
    PCredHandle _phcred;
};

/**
 * Manages the SSL write state machine.
 */
class SSLWriteManager {
public:
    SSLWriteManager(PCtxtHandle hctxt, ReusableBuffer* pOutBuffer)
        : _phctxt(hctxt), _pOutBuffer(pOutBuffer) {}

    /**
     * Encrypts data to be sent to the remote side.
     *
     * If the message is >= max packet side, it will return want_output_and_retry, and expects
     * callers to continue to call it with the same parameters until want_output is returned.
     */
    ssl_want writeUnencryptedData(const void* pMessage,
                                  std::size_t messageLength,
                                  std::size_t& bytes_transferred,
                                  asio::error_code& ec);

    /**
     * Read encrypted data to be sent to the remote side.
     */
    void readOutputBuffer(void* data, size_t inLength, size_t& outLength) {
        _pOutBuffer->readInto(data, inLength, outLength);
    }

private:
    ssl_want encryptMessage(const void* pMessage,
                            std::size_t messageLength,
                            std::size_t& bytes_transferred,
                            asio::error_code& ec);

private:
    // Buffer of data to send to the remote side
    ReusableBuffer* _pOutBuffer;

    // SChannel context handle
    PCtxtHandle _phctxt;

    // Position to start encrypting from for messages needing fragmentation
    std::size_t _lastWriteOffset{0};

    // TLS packet header length
    std::size_t _securityHeaderLength{ULONG_MAX};

    // TLS max packet size - 16kb typically
    std::size_t _securityMaxMessageLength{ULONG_MAX};

    // TLS packet trailer length
    std::size_t _securityTrailerLength{ULONG_MAX};
};

#include "asio/detail/pop_options.hpp"

}  // namespace detail
}  // namespace ssl
}  // namespace asio
