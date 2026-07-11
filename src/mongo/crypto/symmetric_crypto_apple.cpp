// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/crypto/symmetric_crypto.h"
#include "mongo/crypto/symmetric_key.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <set>

#include <CommonCrypto/CommonCryptor.h>
#include <Security/Security.h>

namespace mongo {
namespace crypto {

namespace {
template <typename Parent>
class SymmetricImplApple : public Parent {
public:
    SymmetricImplApple(const SymmetricKey& key, aesMode mode, ConstDataRange iv)
        : _ctx(nullptr, CCCryptorRelease) {
        static_assert(
            std::is_same<Parent, SymmetricEncryptor>::value ||
                std::is_same<Parent, SymmetricDecryptor>::value,
            "SymmetricImplApple must inherit from SymmetricEncryptor or SymmetricDecryptor");

        CCMode ccMode;
        if (mode == aesMode::cbc) {
            ccMode = kCCModeCBC;
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Invalid ivlen for selected algorithm, expected "
                                  << aesCBCIVSize << ", got " << static_cast<int>(iv.length()),
                    iv.length() == aesCBCIVSize);
        } else if (mode == aesMode::ctr) {
            ccMode = kCCModeCTR;
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Invalid ivlen for selected algorithm, expected "
                                  << aesCTRIVSize << ", got " << static_cast<int>(iv.length()),
                    iv.length() == aesCTRIVSize);
        } else {
            uassert(ErrorCodes::UnsupportedFormat,
                    "Native crypto on this platform only supports AES256-CBC or AES256-CTR",
                    false);
        }

        CCCryptorRef context = nullptr;
        constexpr auto op =
            std::is_same<Parent, SymmetricEncryptor>::value ? kCCEncrypt : kCCDecrypt;
        constexpr void* tweak = nullptr;
        constexpr size_t tweakLength = 0;
        constexpr int numRounds = 0;
        constexpr CCModeOptions ccModeOptions = 0;
        const auto status = CCCryptorCreateWithMode(op,
                                                    ccMode,
                                                    kCCAlgorithmAES,
                                                    kCCOptionPKCS7Padding,
                                                    iv.data<std::uint8_t>(),
                                                    key.getKey(),
                                                    key.getKeySize(),
                                                    tweak,
                                                    tweakLength,
                                                    numRounds,
                                                    ccModeOptions,
                                                    &context);
        uassert(ErrorCodes::UnknownError,
                str::stream() << "CCCryptorCreate failure: " << status,
                status == kCCSuccess);

        _ctx.reset(context);
    }

    StatusWith<std::size_t> update(ConstDataRange in, DataRange out) final {
        std::size_t outUsed = 0;
        const auto status = CCCryptorUpdate(
            _ctx.get(), in.data(), in.length(), out.data<std::uint8_t>(), out.length(), &outUsed);
        if (status != kCCSuccess) {
            return Status(ErrorCodes::UnknownError,
                          str::stream() << "Unable to perform CCCryptorUpdate: " << status);
        }
        return outUsed;
    }

    Status addAuthenticatedData(ConstDataRange authData) final {
        fassert(51128, authData.length() == 0);
        return Status::OK();
    }

    StatusWith<size_t> finalize(DataRange out) final {
        size_t outUsed = 0;
        const auto status =
            CCCryptorFinal(_ctx.get(), out.data<std::uint8_t>(), out.length(), &outUsed);
        if (status != kCCSuccess) {
            return Status(ErrorCodes::UnknownError,
                          str::stream() << "Unable to perform CCCryptorFinal: " << status);
        }
        return outUsed;
    }

private:
    std::unique_ptr<_CCCryptor, decltype(&CCCryptorRelease)> _ctx;
};

class SymmetricEncryptorApple : public SymmetricImplApple<SymmetricEncryptor> {
public:
    using SymmetricImplApple::SymmetricImplApple;

    StatusWith<std::size_t> finalizeTag(DataRange) final {
        // CBC only, no tag to create.
        return 0;
    }
};


class SymmetricDecryptorApple : public SymmetricImplApple<SymmetricDecryptor> {
public:
    using SymmetricImplApple::SymmetricImplApple;

    Status updateTag(ConstDataRange tag) final {
        // CBC only, no tag to verify.
        if (tag.length() > 0) {
            return {ErrorCodes::BadValue, "Unexpected tag for non-gcm cipher"};
        }
        return Status::OK();
    }
};

}  // namespace

std::set<std::string> getSupportedSymmetricAlgorithms() {
    return {aes256CBCName, aes256CTRName};
}

Status engineRandBytes(DataRange buffer) {
    auto result =
        SecRandomCopyBytes(kSecRandomDefault, buffer.length(), buffer.data<std::uint8_t>());
    if (result != errSecSuccess) {
        return {ErrorCodes::UnknownError,
                str::stream() << "Failed generating random bytes: " << result};
    } else {
        return Status::OK();
    }
}

StatusWith<std::unique_ptr<SymmetricEncryptor>> SymmetricEncryptor::create(const SymmetricKey& key,
                                                                           aesMode mode,
                                                                           ConstDataRange iv) try {
    std::unique_ptr<SymmetricEncryptor> encryptor =
        std::make_unique<SymmetricEncryptorApple>(key, mode, iv);
    return std::move(encryptor);
} catch (const DBException& e) {
    return e.toStatus();
}

StatusWith<std::unique_ptr<SymmetricDecryptor>> SymmetricDecryptor::create(const SymmetricKey& key,
                                                                           aesMode mode,
                                                                           ConstDataRange iv) try {
    std::unique_ptr<SymmetricDecryptor> decryptor =
        std::make_unique<SymmetricDecryptorApple>(key, mode, iv);
    return std::move(decryptor);
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace crypto
}  // namespace mongo
