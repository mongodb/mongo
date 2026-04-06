/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/db/extension/host/rnp/rnp.h"

#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <fstream>

#include <fmt/format.h>
#include <rnp/rnp_err.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExtension

namespace mongo::extension::host::rnp {

// Evaluates an RNP call, and if it fails, tasserts/uasserts with the RNP error code and
// description included in the message.
#define RNP_TASSERT(errorCode, msg, expr)                                      \
    do {                                                                       \
        auto rnpResult_ = (expr);                                              \
        if (rnpResult_ != RNP_SUCCESS) {                                       \
            tasserted(errorCode,                                               \
                      fmt::format("RnpError: msg={}, code={}, description={}", \
                                  msg,                                         \
                                  rnpResult_,                                  \
                                  rnp_result_to_string(rnpResult_)));          \
        }                                                                      \
    } while (0)

#define RNP_UASSERT(errorCode, msg, expr)                                      \
    do {                                                                       \
        auto rnpResult_ = (expr);                                              \
        if (rnpResult_ != RNP_SUCCESS) {                                       \
            uasserted(errorCode,                                               \
                      fmt::format("RnpError: msg={}, code={}, description={}", \
                                  msg,                                         \
                                  rnpResult_,                                  \
                                  rnp_result_to_string(rnpResult_)));          \
        }                                                                      \
    } while (0)

namespace {
struct RnpKeyHandle {
    ~RnpKeyHandle() {
        if (key) {
            rnp_key_handle_destroy(key);
        }
    }
    rnp_key_handle_t key = nullptr;
};

struct RnpBufferHandle {
    ~RnpBufferHandle() {
        if (buffer) {
            rnp_buffer_destroy(buffer);
        }
    }
    char* buffer = nullptr;
};

class RnpVerificationContext {
public:
    RnpVerificationContext() {};

    ~RnpVerificationContext() {
        if (_verifyOp) {
            if (rnp_op_verify_destroy(_verifyOp) != RNP_SUCCESS) {
                LOGV2_DEBUG(11528915, 4, "Failed to destroy RNP verify op!");
            }
        }
    }

    static RnpVerificationContext createForDetachedSignature(const RnpContext& rnp,
                                                             const RnpInput& signedInput,
                                                             const RnpInput& signatureInput) {
        RnpVerificationContext verificationCtx;
        RNP_TASSERT(11528908,
                    "Failed to create verification context!",
                    rnp_op_verify_detached_create(
                        &verificationCtx._verifyOp, *rnp, *signedInput, *signatureInput));
        RNP_TASSERT(11528921,
                    "Failed to set flags on verification context!",
                    rnp_op_verify_set_flags(*verificationCtx, RNP_VERIFY_REQUIRE_ALL_SIGS));
        return verificationCtx;
    }

    void executeVerify() const {
        RNP_UASSERT(11528917,
                    "Failed to execute verification operation!",
                    rnp_op_verify_execute(_verifyOp));
    }

    rnp_op_verify_t operator*() const {
        return _verifyOp;
    }

    size_t getVerifiedSignatureCount() const {
        size_t sigcount = 0;
        RNP_TASSERT(11528910,
                    "Failed to get signature count during verification.",
                    rnp_op_verify_get_signature_count(_verifyOp, &sigcount));
        return sigcount;
    }

private:
    rnp_op_verify_t _verifyOp{nullptr};
};
}  // namespace

RnpInput::~RnpInput() {
    if (_input) {
        if (rnp_input_destroy(_input) != RNP_SUCCESS) {
            LOGV2_DEBUG(11528914, 4, "Failed to destroy RNP input!");
        }
    }
}

RnpContext::~RnpContext() {
    if (_ffi) {
        auto result = rnp_ffi_destroy(_ffi);
        if (result != RNP_SUCCESS) {
            LOGV2_DEBUG(11528907, 4, "Failed to destroy RNP ffi!");
        }
    }
}

void RnpContext::initialize() {
    RNP_TASSERT(11528909, "Failed to create RNP ffi!", rnp_ffi_create(&_ffi, "GPG", "GPG"));
}

void RnpContext::verifyDetachedSignature(const std::string& signedDataFilePath,
                                         const std::string& signatureFilePath) const {
    tassert(11528918,
            "Failed to verify detached signatures, context was not initialized",
            _ffi != nullptr);

    auto signedInput = RnpInput::createFromPath(signedDataFilePath);
    auto signatureInput = RnpInput::createFromPath(signatureFilePath);

    auto verifyCtx =
        RnpVerificationContext::createForDetachedSignature(*this, signedInput, signatureInput);

    verifyCtx.executeVerify();

    // Verify we had exactly one valid signature and it matched our keyring.
    const auto sigcount = verifyCtx.getVerifiedSignatureCount();
    uassert(
        11528922,
        fmt::format("Failed to detect verification signature, expected {} signatures, but got {}",
                    _numImportedKeys,
                    sigcount),
        sigcount == _numImportedKeys && _numImportedKeys == 1u);

    for (size_t sigIdx = 0; sigIdx < sigcount; ++sigIdx) {
        // Note: sig's lifetime is bound to verifyCtx, so we don't manage its memory.
        rnp_op_verify_signature_t sig = nullptr;
        RNP_TASSERT(11528911,
                    "Failed to get signature during verification.",
                    rnp_op_verify_get_signature_at(*verifyCtx, sigIdx, &sig));
        RnpKeyHandle key;
        RNP_UASSERT(11528912,
                    "Failed to get signature key during verification.",
                    rnp_op_verify_signature_get_key(sig, &key.key));
        RnpBufferHandle keyid;
        RNP_TASSERT(11528913,
                    "Failed to get signature key id during verification.",
                    rnp_key_get_keyid(key.key, &keyid.buffer));
        RNP_UASSERT(11528916,
                    "Failed to verify signature status.",
                    rnp_op_verify_signature_get_status(sig));
    }
}

rnp_ffi_t RnpContext::operator*() const {
    return _ffi;
}

RnpInput RnpInput::createFromPath(const std::filesystem::path& absolutePath) {
    RnpInput input;
    RNP_UASSERT(11528900,
                "Failed to create RNP input from path, operation failed.",
                rnp_input_from_path(&input._input, absolutePath.c_str()));
    tassert(11528901, "Failed to create RNP input from path, null input.", input._input);
    return input;
}

RnpInput RnpInput::createFromMemory(const std::string& contents, bool doCopy /* = false */) {
    RnpInput input;
    RNP_UASSERT(11528902,
                "Failed to create RNP input from memory, operation failed.",
                rnp_input_from_memory(&input._input,
                                      reinterpret_cast<const uint8_t*>(contents.c_str()),
                                      contents.length(),
                                      doCopy));
    tassert(11528903, "Failed to create RNP input from memory, null input.", input._input);
    return input;
}

void RnpContext::importKey(const RnpInput& key) {
    uassert(
        11528928, "Attempted to import more than one key into RnpContext", _numImportedKeys == 0);
    RNP_UASSERT(11528906,
                "Failed to import validation key into rnp context.",
                rnp_import_keys(
                    _ffi, *key, RNP_LOAD_SAVE_PUBLIC_KEYS | RNP_LOAD_SAVE_SECRET_KEYS, nullptr));
    ++_numImportedKeys;
}

RnpOutput::RnpOutput() {
    RNP_TASSERT(11528924, "Failed to create RnpOutput", rnp_output_to_memory(&_output, 0));
}

RnpOutput::~RnpOutput() {
    if (_output) {
        if (rnp_output_destroy(_output) != RNP_SUCCESS) {
            LOGV2_DEBUG(11528927, 4, "Failed to destroy RNP output!");
        }
    }
}

void RnpOutput::pipeFromInput(const RnpInput& input) {
    RNP_TASSERT(
        11528925, "Failed to pipe RnpInput to RnpOutput.", rnp_output_pipe(*input, _output));

    RNP_TASSERT(11528926,
                "Failed to get RnpOutput data buffer.",
                rnp_output_memory_get_buf(_output, &_buf, &_bufLen, false));
}

std::string_view RnpOutput::getAsStringView() const {
    return std::string_view{reinterpret_cast<const char*>(_buf), _bufLen};
}
}  // namespace mongo::extension::host::rnp
