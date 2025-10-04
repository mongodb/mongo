/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/ossl_typ.h>
#include <openssl/rand.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace crypto {


/**
 * Class to load singleton instances of each Encryption Cipher algorithm.
 */
#if OPENSSL_VERSION_NUMBER > 0x30000000L

class OpenSSLCipherLoader {
public:
    OpenSSLCipherLoader() {
        _algoAES256CBC = EVP_CIPHER_fetch(NULL, "AES-256-CBC", NULL);
        _algoAES256GCM = EVP_CIPHER_fetch(NULL, "AES-256-GCM", NULL);
        _algoAES256CTR = EVP_CIPHER_fetch(NULL, "AES-256-CTR", NULL);
    }

    ~OpenSSLCipherLoader() {
        EVP_CIPHER_free(_algoAES256CBC);
        EVP_CIPHER_free(_algoAES256GCM);
        EVP_CIPHER_free(_algoAES256CTR);
    }

    const EVP_CIPHER* getAES256CTR() {
        return _algoAES256CTR;
    }

    const EVP_CIPHER* getAES256GCM() {
        return _algoAES256GCM;
    }

    const EVP_CIPHER* getAES256CBC() {
        return _algoAES256CBC;
    }

private:
    EVP_CIPHER* _algoAES256CTR;
    EVP_CIPHER* _algoAES256GCM;
    EVP_CIPHER* _algoAES256CBC;
};
#else

class OpenSSLCipherLoader {
public:
    OpenSSLCipherLoader() {
        _algoAES256CBC = EVP_get_cipherbyname("aes-256-cbc");
        _algoAES256GCM = EVP_get_cipherbyname("aes-256-gcm");
        _algoAES256CTR = EVP_get_cipherbyname("aes-256-ctr");
    }

    const EVP_CIPHER* getAES256CTR() {
        return _algoAES256CTR;
    }

    const EVP_CIPHER* getAES256GCM() {
        return _algoAES256GCM;
    }

    const EVP_CIPHER* getAES256CBC() {
        return _algoAES256CBC;
    }

private:
    const EVP_CIPHER* _algoAES256CTR;
    const EVP_CIPHER* _algoAES256GCM;
    const EVP_CIPHER* _algoAES256CBC;
};

#endif

static OpenSSLCipherLoader& getOpenSSLCipherLoader() {
    static OpenSSLCipherLoader* loader = new OpenSSLCipherLoader();
    return *loader;
}


namespace {
template <typename Init>
void initCipherContext(
    EVP_CIPHER_CTX* ctx, const SymmetricKey& key, aesMode mode, ConstDataRange iv, Init init) {
    const auto keySize = key.getKeySize();
    const EVP_CIPHER* cipher = nullptr;
    if (keySize == sym256KeySize) {
        if (mode == crypto::aesMode::cbc) {
            cipher = getOpenSSLCipherLoader().getAES256CBC();
        } else if (mode == crypto::aesMode::gcm) {
            cipher = getOpenSSLCipherLoader().getAES256GCM();
        } else if (mode == crypto::aesMode::ctr) {
            cipher = getOpenSSLCipherLoader().getAES256CTR();
        }
    }
    uassert(ErrorCodes::BadValue,
            str::stream() << "Unrecognized AES key size/cipher mode. Size: " << keySize
                          << " Mode: " << getStringFromCipherMode(mode),
            cipher);

    const bool initOk = (1 == init(ctx, cipher, nullptr, key.getKey(), iv.data<std::uint8_t>()));
    uassert(ErrorCodes::UnknownError,
            str::stream() << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()),
            initOk);
}

class SymmetricEncryptorOpenSSL : public SymmetricEncryptor {
public:
    SymmetricEncryptorOpenSSL(const SymmetricKey& key, aesMode mode, ConstDataRange iv)
        : _ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free), _mode(mode) {
        initCipherContext(_ctx.get(), key, mode, iv, EVP_EncryptInit_ex);
    }

    StatusWith<std::size_t> update(ConstDataRange in, DataRange out) final {
        size_t cipherBlockSize = EVP_CIPHER_CTX_block_size(_ctx.get());


        if (out.data() == nullptr) {
            // Presumed intentional null output buffer
            invariant(out.length() == 0);
        } else {
            // Data is padded to the next multiple of cipherBlockSize
            size_t minimumOutputSize = in.length();
            if (auto remainder = in.length() % cipherBlockSize) {
                minimumOutputSize += cipherBlockSize - remainder;
            }

            if (out.length() < minimumOutputSize) {
                return Status(ErrorCodes::Overflow,
                              str::stream() << "Write buffer too small for Encryptor update: "
                                            << static_cast<int>(out.length()));
            }
        }

        int len = 0;
        if (1 !=
            EVP_EncryptUpdate(
                _ctx.get(), out.data<std::uint8_t>(), &len, in.data<std::uint8_t>(), in.length())) {
            return Status(ErrorCodes::UnknownError,
                          str::stream()
                              << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
        }
        return static_cast<size_t>(len);
    }

    Status addAuthenticatedData(ConstDataRange in) final {
        fassert(51126, _mode == crypto::aesMode::gcm);

        auto swUpdate = update(in, {nullptr, 0});
        if (!swUpdate.isOK()) {
            return swUpdate.getStatus();
        }

        const auto len = swUpdate.getValue();
        if (len != in.length()) {
            return {ErrorCodes::InternalError,
                    str::stream() << "Unexpected write length while appending AAD: "
                                  << static_cast<int>(len)};
        }

        return Status::OK();
    }

    StatusWith<std::size_t> finalize(DataRange out) final {

        size_t cipherBlockSize = EVP_CIPHER_CTX_block_size(_ctx.get());

        if (cipherBlockSize > 1 && out.length() < cipherBlockSize) {
            return Status(ErrorCodes::Overflow,
                          str::stream() << "Write buffer too small for Encryptor finalize: "
                                        << static_cast<int>(out.length()));
        }
        int len = 0;
        if (1 != EVP_EncryptFinal_ex(_ctx.get(), out.data<std::uint8_t>(), &len)) {
            return Status(ErrorCodes::UnknownError,
                          str::stream()
                              << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
        }
        return static_cast<size_t>(len);
    }

    StatusWith<std::size_t> finalizeTag(DataRange out) final {
        if (_mode == aesMode::gcm) {
#ifdef EVP_CTRL_GCM_GET_TAG
            if (1 !=
                EVP_CIPHER_CTX_ctrl(
                    _ctx.get(), EVP_CTRL_GCM_GET_TAG, out.length(), out.data<std::uint8_t>())) {
                return Status(ErrorCodes::UnknownError,
                              str::stream()
                                  << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
            }
            return crypto::aesGCMTagSize;
#else
            return Status(ErrorCodes::UnsupportedFormat, "GCM support is not available");
#endif
        }

        // Otherwise, not a tagged cipher mode, write nothing.
        return 0;
    }

private:
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> _ctx;
    const aesMode _mode;
};

class SymmetricDecryptorOpenSSL : public SymmetricDecryptor {
public:
    SymmetricDecryptorOpenSSL(const SymmetricKey& key, aesMode mode, ConstDataRange iv)
        : _ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free), _mode(mode) {
        initCipherContext(_ctx.get(), key, mode, iv, EVP_DecryptInit_ex);
    }

    StatusWith<std::size_t> update(ConstDataRange in, DataRange out) final {
        int len = 0;

        if (out.data() == nullptr) {
            // Presumed intentional null output buffer
            invariant(out.length() == 0);
        } else {

            size_t minimumOutputSize = in.length();
            size_t cipherBlockSize = EVP_CIPHER_CTX_block_size(_ctx.get());
            if (in.length() % cipherBlockSize) {
                minimumOutputSize += cipherBlockSize;
            }

            if (out.length() < minimumOutputSize) {
                return Status(ErrorCodes::Overflow,
                              str::stream() << "Write buffer too small for Decryptor update: "
                                            << static_cast<int>(out.length()));
            }
        }

        if (1 !=
            EVP_DecryptUpdate(
                _ctx.get(), out.data<std::uint8_t>(), &len, in.data<std::uint8_t>(), in.length())) {
            return Status(ErrorCodes::UnknownError,
                          str::stream()
                              << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
        }
        return static_cast<std::size_t>(len);
    }

    Status addAuthenticatedData(ConstDataRange authData) final {
        fassert(51125, _mode == crypto::aesMode::gcm);

        auto swUpdate = update(authData, {nullptr, 0});
        if (!swUpdate.isOK()) {
            return swUpdate.getStatus();
        }

        const auto len = swUpdate.getValue();
        if (len != authData.length()) {
            return {ErrorCodes::InternalError,
                    str::stream() << "Unexpected write length while appending AAD: "
                                  << static_cast<int>(len)};
        }

        return Status::OK();
    }

    StatusWith<std::size_t> finalize(DataRange out) final {
        int len = 0;

        size_t cipherBlockSize = EVP_CIPHER_CTX_block_size(_ctx.get());
        if (cipherBlockSize > 1 && out.length() < cipherBlockSize) {
            return Status(ErrorCodes::Overflow,
                          str::stream() << "Write buffer too small for Encryptor finalize: "
                                        << static_cast<int>(out.length()));
        }

        if (1 != EVP_DecryptFinal_ex(_ctx.get(), out.data<std::uint8_t>(), &len)) {
            return Status(ErrorCodes::UnknownError,
                          str::stream()
                              << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
        }
        return static_cast<std::size_t>(len);
    }

    Status updateTag(ConstDataRange tag) final {
        // validateEncryptionOption asserts that platforms without GCM will never start in GCM mode
        if (_mode == aesMode::gcm) {
#ifdef EVP_CTRL_GCM_GET_TAG
            if (1 !=
                EVP_CIPHER_CTX_ctrl(_ctx.get(),
                                    EVP_CTRL_GCM_SET_TAG,
                                    tag.length(),
                                    const_cast<std::uint8_t*>(tag.data<std::uint8_t>()))) {
                return Status(ErrorCodes::UnknownError,
                              str::stream()
                                  << "Unable to set GCM tag: "
                                  << SSLManagerInterface::getSSLErrorMessage(ERR_get_error()));
            }
#else
            return {ErrorCodes::UnsupportedFormat, "GCM support is not available"};
#endif
        } else if (tag.length() != 0) {
            return {ErrorCodes::BadValue, "Unexpected tag for non-gcm cipher"};
        }

        return Status::OK();
    }

private:
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> _ctx;
    const aesMode _mode;
};

}  // namespace

std::set<std::string> getSupportedSymmetricAlgorithms() {
#if defined(EVP_CTRL_GCM_GET_TAG) && !defined(__APPLE__)
    return {aes256CBCName, aes256GCMName, aes256CTRName};
#else
    return {aes256CBCName, aes256CTRName};
#endif
}

Status engineRandBytes(DataRange buffer) {
    if (RAND_bytes(buffer.data<std::uint8_t>(), buffer.length()) == 1) {
        return Status::OK();
    }
    return {ErrorCodes::UnknownError,
            str::stream() << "Unable to acquire random bytes from OpenSSL: "
                          << SSLManagerInterface::getSSLErrorMessage(ERR_get_error())};
}

StatusWith<std::unique_ptr<SymmetricEncryptor>> SymmetricEncryptor::create(const SymmetricKey& key,
                                                                           aesMode mode,
                                                                           ConstDataRange iv) try {
    std::unique_ptr<SymmetricEncryptor> encryptor =
        std::make_unique<SymmetricEncryptorOpenSSL>(key, mode, iv);
    return std::move(encryptor);
} catch (const DBException& e) {
    return e.toStatus();
}

StatusWith<std::unique_ptr<SymmetricDecryptor>> SymmetricDecryptor::create(const SymmetricKey& key,
                                                                           aesMode mode,
                                                                           ConstDataRange iv) try {
    std::unique_ptr<SymmetricDecryptor> decryptor =
        std::make_unique<SymmetricDecryptorOpenSSL>(key, mode, iv);
    return std::move(decryptor);
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace crypto
}  // namespace mongo
