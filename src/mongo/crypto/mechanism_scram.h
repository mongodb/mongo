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

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/secure_allocator.h"
#include "mongo/base/status.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace scram {

constexpr auto kServerKeyConst = "Server Key"_sd;
constexpr auto kClientKeyConst = "Client Key"_sd;

constexpr auto kIterationCountFieldName = "iterationCount"_sd;
constexpr auto kSaltFieldName = "salt"_sd;
constexpr auto kStoredKeyFieldName = "storedKey"_sd;
constexpr auto kServerKeyFieldName = "serverKey"_sd;

const int kIterationCountMinimum = 4096;

/* The precursors necessary to perform the computation which produces SCRAMSecrets.
 * These are the original password, its salt, and the number of times it must be
 * hashed to produce the SaltedPassword used to generate the rest of the SCRAMSecrets.
 */
template <typename HashBlock>
class Presecrets {
public:
    Presecrets(std::string password, std::vector<std::uint8_t> salt, size_t iterationCount)
        : _password(std::move(password)), _salt(std::move(salt)), _iterationCount(iterationCount) {
        uassert(17450, "invalid salt length provided", _salt.size() == saltLength());
        uassert(50662, "invalid iteration count", _iterationCount >= kIterationCountMinimum);
    }

    HashBlock generateSaltedPassword() const noexcept {
        // saltedPassword = Hi(hashedPassword, salt)

        // Reserve a HashBlock::kHashLength block for the initial key.
        // We use saltLength() salts, and reserve the extra for a suffix mandated by RFC5802.
        std::array<std::uint8_t, HashBlock::kHashLength> startKey;
        std::copy(_salt.cbegin(), _salt.cend(), startKey.begin());
        startKey[_salt.size() + 0] = 0;
        startKey[_salt.size() + 1] = 0;
        startKey[_salt.size() + 2] = 0;
        startKey[_salt.size() + 3] = 1;

        // U1 = HMAC(input, salt + 0001)
        auto output =
            HashBlock::computeHmac(reinterpret_cast<const unsigned char*>(_password.c_str()),
                                   _password.size(),
                                   startKey.data(),
                                   startKey.size());
        auto intermediate = output;

        // intermediateDigest contains Ui and output contains the accumulated XOR:ed result
        invariant(_iterationCount >= kIterationCountMinimum);
        for (size_t i = 1; i < _iterationCount; ++i) {
            intermediate =
                HashBlock::computeHmac(reinterpret_cast<const unsigned char*>(_password.c_str()),
                                       _password.size(),
                                       intermediate.data(),
                                       intermediate.size());
            output.xorInline(intermediate);
        }

        return output;
    }

    static std::vector<std::uint8_t> generateSecureRandomSalt() {
        // Express salt length as a number of quad words, rounded up.
        constexpr auto qwords = (saltLength() + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        std::array<std::int64_t, qwords> userSalt;

        std::unique_ptr<SecureRandom> sr(SecureRandom::create());
        std::generate(userSalt.begin(), userSalt.end(), [&sr] { return sr->nextInt64(); });
        return std::vector<std::uint8_t>(reinterpret_cast<std::uint8_t*>(userSalt.data()),
                                         reinterpret_cast<std::uint8_t*>(userSalt.data()) +
                                             saltLength());
    }

private:
    template <typename T>
    friend bool operator==(const Presecrets<T>&, const Presecrets<T>&);

    auto equalityLens() const {
        return std::tie(_password, _salt, _iterationCount);
    }

    static constexpr auto saltLength() -> decltype(HashBlock::kHashLength) {
        return HashBlock::kHashLength - 4;
    }

    std::string _password;
    std::vector<std::uint8_t> _salt;
    size_t _iterationCount;
};

template <typename T>
bool operator==(const Presecrets<T>& lhs, const Presecrets<T>& rhs) {
    return lhs.equalityLens() == rhs.equalityLens();
}
template <typename T>
bool operator!=(const Presecrets<T>& lhs, const Presecrets<T>& rhs) {
    return !(lhs == rhs);
}
template <typename HashBlock>
struct SecretsHolder {
    HashBlock clientKey;
    HashBlock storedKey;
    HashBlock serverKey;
};

template <typename HashBlock>
class LockedSecretsPolicy {
public:
    LockedSecretsPolicy() = default;

    const SecretsHolder<HashBlock>* operator->() const {
        return &(*_holder);
    }
    SecretsHolder<HashBlock>* operator->() {
        return &(*_holder);
    }

private:
    using SecureSecrets = SecureAllocatorAuthDomain::SecureHandle<SecretsHolder<HashBlock>>;

    SecureSecrets _holder;
};

template <typename HashBlock>
class UnlockedSecretsPolicy {
public:
    UnlockedSecretsPolicy() = default;

    const SecretsHolder<HashBlock>* operator->() const {
        return &_holder;
    }

    SecretsHolder<HashBlock>* operator->() {
        return &_holder;
    }

private:
    SecretsHolder<HashBlock> _holder;
};

/* Stores all of the keys, generated from a password, needed for a client or server to perform a
 * SCRAM handshake.
 * These keys are reference counted, and allocated using the SecureAllocator.
 * May be unpopulated. SCRAMSecrets created via the default constructor are unpopulated.
 * The behavior is undefined if the accessors are called when unpopulated.
 */
template <typename HashBlock, template <typename> class MemoryPolicy = LockedSecretsPolicy>
class Secrets {
public:
    Secrets() = default;

    Secrets(StringData client, StringData stored, StringData server)
        : _ptr(std::make_shared<MemoryPolicy<HashBlock>>()) {
        if (!client.empty()) {
            (*_ptr)->clientKey = uassertStatusOK(HashBlock::fromBuffer(
                reinterpret_cast<const unsigned char*>(client.rawData()), client.size()));
        }
        (*_ptr)->storedKey = uassertStatusOK(HashBlock::fromBuffer(
            reinterpret_cast<const unsigned char*>(stored.rawData()), stored.size()));
        (*_ptr)->serverKey = uassertStatusOK(HashBlock::fromBuffer(
            reinterpret_cast<const unsigned char*>(server.rawData()), stored.size()));
    }

    Secrets(const HashBlock& saltedPassword) : _ptr(std::make_shared<MemoryPolicy<HashBlock>>()) {
        // ClientKey := HMAC(saltedPassword, "Client Key")
        (*_ptr)->clientKey = (HashBlock::computeHmac(
            saltedPassword.data(),
            saltedPassword.size(),
            reinterpret_cast<const unsigned char*>(kClientKeyConst.rawData()),
            kClientKeyConst.size()));
        // StoredKey := H(clientKey)
        (*_ptr)->storedKey = HashBlock::computeHash(clientKey().data(), clientKey().size());

        // ServerKey := HMAC(SaltedPassword, "Server Key")
        (*_ptr)->serverKey = HashBlock::computeHmac(
            saltedPassword.data(),
            saltedPassword.size(),
            reinterpret_cast<const unsigned char*>(kServerKeyConst.rawData()),
            kServerKeyConst.size());
    }
    Secrets(const Presecrets<HashBlock>& presecrets)
        : Secrets(presecrets.generateSaltedPassword()) {}

    std::string generateClientProof(StringData authMessage) const {
        // ClientProof := HMAC(StoredKey, AuthMessage) ^ ClientKey
        auto proof =
            HashBlock::computeHmac(storedKey().data(),
                                   storedKey().size(),
                                   reinterpret_cast<const unsigned char*>(authMessage.rawData()),
                                   authMessage.size());
        proof.xorInline(clientKey());
        return proof.toString();
    }
    bool verifyClientProof(StringData authMessage, StringData proof) const {
        // ClientKey := HMAC(StoredKey, AuthMessage) ^ ClientProof
        auto key =
            HashBlock::computeHmac(storedKey().data(),
                                   storedKey().size(),
                                   reinterpret_cast<const unsigned char*>(authMessage.rawData()),
                                   authMessage.size());
        key.xorInline(uassertStatusOK(HashBlock::fromBuffer(
            reinterpret_cast<const uint8_t*>(proof.rawData()), proof.size())));

        // StoredKey := H(ClientKey)
        auto exp = HashBlock::computeHash(key.data(), key.size());

        if ((exp.size() != HashBlock::kHashLength) ||
            (storedKey().size() != HashBlock::kHashLength)) {
            return false;
        }

        return consttimeMemEqual(reinterpret_cast<const unsigned char*>(exp.data()),
                                 storedKey().data(),
                                 HashBlock::kHashLength);
    }
    std::string generateServerSignature(StringData authMessage) const {
        // ServerSignature := HMAC(ServerKey, AuthMessage)
        return HashBlock::computeHmac(serverKey().data(),
                                      serverKey().size(),
                                      reinterpret_cast<const unsigned char*>(authMessage.rawData()),
                                      authMessage.size())
            .toString();
    }
    bool verifyServerSignature(StringData authMessage, StringData sig) const {
        // ServerSignature := HMAC(ServerKey, AuthMessage)
        const auto exp =
            HashBlock::computeHmac(serverKey().data(),
                                   serverKey().size(),
                                   reinterpret_cast<const unsigned char*>(authMessage.rawData()),
                                   authMessage.size());

        if ((sig.size() != HashBlock::kHashLength) || (exp.size() != HashBlock::kHashLength)) {
            return false;
        }
        return consttimeMemEqual(reinterpret_cast<const unsigned char*>(sig.rawData()),
                                 reinterpret_cast<const unsigned char*>(exp.data()),
                                 HashBlock::kHashLength);
    }

    static BSONObj generateCredentials(std::string password, int iterationCount) {
        auto salt = Presecrets<HashBlock>::generateSecureRandomSalt();
        return generateCredentials(salt, password, iterationCount);
    }

    static BSONObj generateCredentials(const std::vector<uint8_t>& salt,
                                       const std::string& password,
                                       int iterationCount) {
        Secrets<HashBlock, MemoryPolicy> secrets(
            Presecrets<HashBlock>(password, salt, iterationCount));
        const auto encodedSalt =
            base64::encode(reinterpret_cast<const char*>(salt.data()), salt.size());
        return BSON(kIterationCountFieldName << iterationCount << kSaltFieldName << encodedSalt
                                             << kStoredKeyFieldName
                                             << secrets.storedKey().toString()
                                             << kServerKeyFieldName
                                             << secrets.serverKey().toString());
    }

    const HashBlock& clientKey() const {
        auto& ret = (*_ptr)->clientKey;
        uassert(
            ErrorCodes::BadValue, "Invalid SCRAM client key", ret.size() == HashBlock::kHashLength);
        return ret;
    }
    const HashBlock& storedKey() const {
        auto& ret = (*_ptr)->storedKey;
        uassert(
            ErrorCodes::BadValue, "Invalid SCRAM stored key", ret.size() == HashBlock::kHashLength);
        return ret;
    }
    const HashBlock& serverKey() const {
        auto& ret = (*_ptr)->serverKey;
        uassert(
            ErrorCodes::BadValue, "Invalid SCRAM server key", ret.size() == HashBlock::kHashLength);
        return ret;
    }

    operator bool() const {
        return (bool)_ptr;
    }

private:
    std::shared_ptr<MemoryPolicy<HashBlock>> _ptr;
};

}  // namespace scram
}  // namespace mongo
