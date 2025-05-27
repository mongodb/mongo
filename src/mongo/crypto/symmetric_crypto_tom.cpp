/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/base/data_cursor.h"
#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/config.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <vector>

#include <tomcrypt.h>

#include <sys/types.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


#ifdef MONGO_CONFIG_SSL
#error This file should not be included if compiling with SSL support
#endif

namespace mongo {
namespace crypto {

namespace {

/**
 * Since tomcrypt only does block aligned crypto, we have to buffer bytes into blocks.
 *
 * This is implementation is simple but inefficient. It only needs to support the non-ssl build
 * which is not shipped to customers.
 *
 * Also, tomcrypt does not support padding in 1.8.2 so we implement PKCS#7 padding.
 */
class BufferedCryptoStream {
public:
    virtual ~BufferedCryptoStream() = default;

    virtual StatusWith<std::size_t> doBlockAlignedOperation(DataRange buf, size_t size) = 0;

    StatusWith<std::size_t> addData(ConstDataRange cdr, DataRange out, bool encrypt) {
        // Add all the data in
        std::copy(cdr.data(), cdr.data() + cdr.length(), std::back_inserter(_buffer));

        // Encrypt/decrypt all the blocks we can
        size_t blocks = _buffer.size() / aesBlockSize;
        size_t payloadSize = blocks * aesBlockSize;

        if (payloadSize > 0) {
            auto swOp = doBlockAlignedOperation(_buffer, payloadSize);
            if (!swOp.isOK()) {
                return swOp;
            }

            // For decryption, save the last block as it is supposed a block of pads
            if (!encrypt) {
                payloadSize -= aesBlockSize;
            }

            memcpy(out.data(), _buffer.data(), payloadSize);

            // Remove everything that we just encrypted/decrypted
            _buffer.erase(_buffer.begin(), _buffer.begin() + payloadSize);

            return payloadSize;
        } else {
            return 0;
        }
    }

    StatusWith<std::size_t> finalizeBufferForDecryption(DataRange out) {
        if (_buffer.size() != aesBlockSize) {
            return Status(ErrorCodes::BadValue, "invalid final block buffer");
        }

        uint8_t pad = _buffer[_buffer.size() - 1];
        if (pad == 0 || pad > aesBlockSize) {
            return Status(ErrorCodes::BadValue, "wrong pad length");
        }

        // Validate pad
        for (size_t i = 0; i < pad; i++) {
            if (_buffer[aesBlockSize - i - 1] != pad) {
                return Status(ErrorCodes::BadValue, "wrong pad byte");
            }
        }

        size_t final_size = aesBlockSize - pad;
        if (final_size == 0) {
            return 0;
        }

        memcpy(out.data(), _buffer.data(), final_size);

        return {final_size};
    }

    StatusWith<std::size_t> finalizeBufferForEncryption(DataRange out) {
        std::array<uint8_t, aesBlockSize> buffer;

        if (_buffer.size() == 0) {
            buffer.fill(aesBlockSize);
        } else {
            invariant(_buffer.size() < aesBlockSize);

            // Pad
            std::copy(_buffer.data(), _buffer.data() + _buffer.size(), buffer.begin());

            uint8_t padding_pkcs7 = aesBlockSize - _buffer.size();
            uint8_t start = _buffer.size();
            for (std::size_t i = 0; i < padding_pkcs7; i++) {
                buffer[start + i] = padding_pkcs7;
            }
        }

        auto swOp = doBlockAlignedOperation(buffer, aesBlockSize);
        if (!swOp.isOK()) {
            return swOp;
        }

        memcpy(out.data(), buffer.data(), aesBlockSize);

        return {buffer.size()};
    }

private:
    std::vector<uint8_t> _buffer;
};

class TomCryptSetup {
public:
    TomCryptSetup() {
        invariant(register_cipher(&aes_desc) != -1);
        _cipher = find_cipher("aes");
    }

    int getCipher() {
        return _cipher;
    }

private:
    int _cipher{0};
};

static TomCryptSetup& getTomCryptSetup() {
    static TomCryptSetup loader;
    return loader;
}

class SymmetricEncryptorTomCrypt : public SymmetricEncryptor, BufferedCryptoStream {
public:
    SymmetricEncryptorTomCrypt(const SymmetricKey& key, aesMode mode, ConstDataRange iv)
        : _mode(mode) {
        if (_mode == crypto::aesMode::cbc) {
            int ret = cbc_start(getTomCryptSetup().getCipher(),
                                iv.data<uint8_t>(),
                                key.getKey(),
                                key.getKeySize(),
                                0,
                                &_contextCBC);
            uassert(6373801, "cbc encrypt init failed", ret == CRYPT_OK);

        } else if (_mode == crypto::aesMode::ctr) {
            int ret = ctr_start(getTomCryptSetup().getCipher(),
                                iv.data<uint8_t>(),
                                key.getKey(),
                                key.getKeySize(),
                                0,
                                CTR_COUNTER_BIG_ENDIAN,
                                &_contextCTR);
            uassert(6373802, "ctr decrypt init failed", ret == CRYPT_OK);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    StatusWith<std::size_t> doBlockAlignedOperation(DataRange buf, size_t size) final {
        if (_mode == crypto::aesMode::cbc) {
            int ret = cbc_encrypt(buf.data<uint8_t>(), buf.data<uint8_t>(), size, &_contextCBC);
            uassert(6373803, "cbc encrypt failed", ret == CRYPT_OK);
        } else {
            uasserted(6373804, "not supported");
        }

        return size;
    }

    StatusWith<std::size_t> update(ConstDataRange in, DataRange out) final {
        if (_mode == crypto::aesMode::cbc) {
            return addData(in, out, true);
        } else if (_mode == crypto::aesMode::ctr) {
            int ret =
                ctr_encrypt(in.data<uint8_t>(), out.data<uint8_t>(), in.length(), &_contextCTR);
            uassert(6373805, "ctr encrypt failed", ret == CRYPT_OK);
        }

        return in.length();
    }

    Status addAuthenticatedData(ConstDataRange in) final {
        fassert(6373806, _mode == crypto::aesMode::gcm);

        return Status::OK();
    }

    StatusWith<std::size_t> finalize(DataRange out) final {
        if (_mode == crypto::aesMode::cbc) {
            auto sw = finalizeBufferForEncryption(out);
            cbc_done(&_contextCBC);
            return sw;
        }

        return 0;
    }

    StatusWith<std::size_t> finalizeTag(DataRange out) final {

        return Status(ErrorCodes::UnsupportedFormat, "GCM support is not available");
    }

private:
    const aesMode _mode;
    symmetric_CBC _contextCBC;
    symmetric_CTR _contextCTR;
};

class SymmetricDecryptorTomCrypt : public SymmetricDecryptor, BufferedCryptoStream {
public:
    SymmetricDecryptorTomCrypt(const SymmetricKey& key, aesMode mode, ConstDataRange iv)
        : _mode(mode) {
        if (_mode == crypto::aesMode::cbc) {
            int ret = cbc_start(getTomCryptSetup().getCipher(),
                                iv.data<uint8_t>(),
                                key.getKey(),
                                key.getKeySize(),
                                0,
                                &_contextCBC);
            uassert(6373807, "cbc decrypt init failed", ret == CRYPT_OK);
        } else if (_mode == crypto::aesMode::ctr) {
            int ret = ctr_start(getTomCryptSetup().getCipher(),
                                iv.data<uint8_t>(),
                                key.getKey(),
                                key.getKeySize(),
                                0,
                                CTR_COUNTER_BIG_ENDIAN,
                                &_contextCTR);
            uassert(6373808, "ctr decrypt init failed", ret == CRYPT_OK);
        } else {
            MONGO_UNREACHABLE;
        }
    }

    StatusWith<std::size_t> doBlockAlignedOperation(DataRange buf, size_t size) final {
        if (_mode == crypto::aesMode::cbc) {
            int ret = cbc_decrypt(buf.data<uint8_t>(), buf.data<uint8_t>(), size, &_contextCBC);
            uassert(6373809, "cbc decrypt failed", ret == CRYPT_OK);
        } else {
            uasserted(6373810, "not supported");
        }

        return size;
    }

    StatusWith<std::size_t> update(ConstDataRange in, DataRange out) final {
        if (_mode == crypto::aesMode::cbc) {
            return addData(in, out, false);
        } else if (_mode == crypto::aesMode::ctr) {
            int ret =
                ctr_decrypt(in.data<uint8_t>(), out.data<uint8_t>(), in.length(), &_contextCTR);
            uassert(6373811, "ctr decrypt failed", ret == CRYPT_OK);
        }

        return in.length();
    }

    Status addAuthenticatedData(ConstDataRange authData) final {
        fassert(6373812, _mode == crypto::aesMode::gcm);

        return Status::OK();
    }

    StatusWith<std::size_t> finalize(DataRange out) final {
        if (_mode == crypto::aesMode::cbc) {
            auto sw = finalizeBufferForDecryption(out);
            cbc_done(&_contextCBC);
            return sw;
        }
        return 0;
    }

    Status updateTag(ConstDataRange tag) final {
        return {ErrorCodes::BadValue, "Unexpected tag for non-gcm cipher"};
    }

private:
    const aesMode _mode;
    symmetric_CBC _contextCBC;
    symmetric_CTR _contextCTR;
};

}  // namespace

std::set<std::string> getSupportedSymmetricAlgorithms() {
    return {aes256CBCName, aes256CTRName};
}

Status engineRandBytes(DataRange buffer) {
    SecureRandom().fill(buffer.data<std::uint8_t>(), buffer.length());
    return Status::OK();
}

StatusWith<std::unique_ptr<SymmetricEncryptor>> SymmetricEncryptor::create(const SymmetricKey& key,
                                                                           aesMode mode,
                                                                           ConstDataRange iv) try {
    std::unique_ptr<SymmetricEncryptor> encryptor =
        std::make_unique<SymmetricEncryptorTomCrypt>(key, mode, iv);
    return std::move(encryptor);
} catch (const DBException& e) {
    return e.toStatus();
}

StatusWith<std::unique_ptr<SymmetricDecryptor>> SymmetricDecryptor::create(const SymmetricKey& key,
                                                                           aesMode mode,
                                                                           ConstDataRange iv) try {
    std::unique_ptr<SymmetricDecryptor> decryptor =
        std::make_unique<SymmetricDecryptorTomCrypt>(key, mode, iv);
    return std::move(decryptor);
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace crypto
}  // namespace mongo
