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

#pragma once

#include <array>
#include <fmt/format.h>
#include <vector>

#include "mongo/base/data_range.h"

namespace mongo {

using PrfBlock = std::array<std::uint8_t, 32>;

/*
 * The many token types are derived from the index key
 *
 * Terminology
 * f = field
 * v = value
 * u =
 *   - For non-contentious fields, we select the partition number, u, to be equal to 0.
 *   - For contentious fields, with a contention factor, p, we pick the partition number, u,
 * uniformly at random from the set {0, ..., p}.
 *
 * CollectionsLevel1Token = HMAC(IndexKey, 1) = K_{f,1}
 * ServerDataEncryptionLevel1Token = HMAC(IndexKey, 3) = K_{f,3} = Fs[f,3]
 *
 * EDCToken = HMAC(CollectionsLevel1Token, 1) = K^{edc}_f = Fs[f,1,1]
 * ESCToken = HMAC(CollectionsLevel1Token, 2) = K^{esc}_f = Fs[f,1,2]
 * ECCToken = HMAC(CollectionsLevel1Token, 3) = K^{ecc}_f = Fs[f,1,3]
 * ECOCToken = HMAC(CollectionsLevel1Token, 4) = K^{ecoc}_f = Fs[f,1,4]
 *
 * EDCDerivedFromDataToken = HMAC(EDCToken, v) = K^{edc}_{f,v} = Fs[f,1,1,v]
 * ESCDerivedFromDataToken = HMAC(ESCToken, v) = K^{esc}_{f,v} = Fs[f,1,2,v]
 * ECCDerivedFromDataToken = HMAC(ECCToken, v) = K^{ecc}_{f,v} = Fs[f,1,3,v]
 *
 * EDCDerivedFromDataTokenAndContentionFactorToken = HMAC(EDCDerivedFromDataToken, u) =
 * Fs[f,1,1,v,u]
 * ESCDerivedFromDataTokenAndContentionFactorToken = HMAC(ESCDerivedFromDataToken, u) =
 * Fs[f,1,2,v,u]
 * ECCDerivedFromDataTokenAndContentionFactorToken = HMAC(ECCDerivedFromDataToken, u) =
 * Fs[f,1,3,v,u]
 *
 * EDCTwiceDerivedToken = HMAC(EDCDerivedFromDataTokenAndContentionFactorToken, 1) = Fs_edc(1)
 * ESCTwiceDerivedTagToken = HMAC(ESCDerivedFromDataTokenAndContentionFactorToken, 1) = Fs_esc(1)
 * ESCTwiceDerivedValueToken = HMAC(ESCDerivedFromDataTokenAndContentionFactorToken, 2) = Fs_esc(2)
 * ECCTwiceDerivedTagToken = HMAC(ECCDerivedFromDataTokenAndContentionFactorToken, 1) = Fs_ecc(1)
 * ECCTwiceDerivedValueToken = HMAC(ECCDerivedFromDataTokenAndContentionFactorToken, 2) = Fs_ecc(2)
 *
 * ServerTokenDerivationLevel1Token = HMAC(IndexKey, 2) = K_{f,2}
 * ServerDerivedFromDataToken = HMAC(ServerTokenDerivationLevel1Token, v) = K_{f,2,v} = Fs[f,2,v]
 * ServerCountAndContentionFactorEncryptionToken = HMAC(ServerDerivedFromDataToken, 1) = Fs[f,2,v,1]
 * ServerZerosEncryptionToken = HMAC(ServerDerivedFromDataToken, 2) = Fs[f,2,v,2]
 *
 * Range Protocol V2
 * AnchorPaddingRootToken = HMAC(ESCToken, d) = S^esc_f_d = Fs[f,1,2,d]
 *  d = 136 bit blob of zero = 17 octets of 0
 * AnchorPaddingKeyToken = HMAC(AnchorPaddingRootToken, 1) = S1_d = F^(S^esc_fd)(1)
 * AnchorPaddingValueToken = HMAC(AnchorPaddingRootToken, 2) =  S2_d = F^(S^esc_fd)(2)
 */
enum class FLETokenType {
    CollectionsLevel1Token,
    ServerDataEncryptionLevel1Token,

    EDCToken,
    ESCToken,
    ECCToken,
    ECOCToken,

    EDCDerivedFromDataToken,
    ESCDerivedFromDataToken,
    ECCDerivedFromDataToken,

    EDCDerivedFromDataTokenAndContentionFactorToken,
    ESCDerivedFromDataTokenAndContentionFactorToken,
    ECCDerivedFromDataTokenAndContentionFactorToken,

    EDCTwiceDerivedToken,
    ESCTwiceDerivedTagToken,
    ESCTwiceDerivedValueToken,
    ECCTwiceDerivedTagToken,
    ECCTwiceDerivedValueToken,

    // v2 tokens
    ServerTokenDerivationLevel1Token,
    ServerDerivedFromDataToken,
    ServerCountAndContentionFactorEncryptionToken,
    ServerZerosEncryptionToken,

    // range protocol v2 tokens
    AnchorPaddingRootToken,
    AnchorPaddingKeyToken,
    AnchorPaddingValueToken,
};

/**
 * Templated C++ class that contains a token. A templated class is used to create a strongly typed
 * API that is hard to misuse.
 */
template <FLETokenType TokenT>
struct FLEToken {
    FLEToken() = default;

    FLEToken(PrfBlock dataIn) : data(std::move(dataIn)) {}

    static FLEToken parse(ConstDataRange block) {
        uassert(9999901, "Invalid prf length", block.length() == sizeof(PrfBlock));

        PrfBlock ret;
        std::copy(block.data(), block.data() + block.length(), ret.data());
        return FLEToken(std::move(ret));
    }

    ConstDataRange toCDR() const {
        return ConstDataRange(data.data(), data.data() + data.size());
    }

    bool operator==(const FLEToken<TokenT>& other) const {
        return (type == other.type) && (data == other.data);
    }

    bool operator!=(const FLEToken<TokenT>& other) const {
        return !(*this == other);
    }

    template <typename H>
    friend H AbslHashValue(H h, const FLEToken<TokenT>& token) {
        return H::combine(std::move(h), token.type, token.data);
    }

    FLETokenType type{TokenT};
    PrfBlock data;
};

using CollectionsLevel1Token = FLEToken<FLETokenType::CollectionsLevel1Token>;
using ServerDataEncryptionLevel1Token = FLEToken<FLETokenType::ServerDataEncryptionLevel1Token>;
using EDCToken = FLEToken<FLETokenType::EDCToken>;
using ESCToken = FLEToken<FLETokenType::ESCToken>;
using ECCToken = FLEToken<FLETokenType::ECCToken>;
using ECOCToken = FLEToken<FLETokenType::ECOCToken>;
using EDCDerivedFromDataToken = FLEToken<FLETokenType::EDCDerivedFromDataToken>;
using ESCDerivedFromDataToken = FLEToken<FLETokenType::ESCDerivedFromDataToken>;
using ECCDerivedFromDataToken = FLEToken<FLETokenType::ECCDerivedFromDataToken>;
using EDCDerivedFromDataTokenAndContentionFactorToken =
    FLEToken<FLETokenType::EDCDerivedFromDataTokenAndContentionFactorToken>;
using ESCDerivedFromDataTokenAndContentionFactorToken =
    FLEToken<FLETokenType::ESCDerivedFromDataTokenAndContentionFactorToken>;
using ECCDerivedFromDataTokenAndContentionFactorToken =
    FLEToken<FLETokenType::ECCDerivedFromDataTokenAndContentionFactorToken>;
using EDCTwiceDerivedToken = FLEToken<FLETokenType::EDCTwiceDerivedToken>;
using ESCTwiceDerivedTagToken = FLEToken<FLETokenType::ESCTwiceDerivedTagToken>;
using ESCTwiceDerivedValueToken = FLEToken<FLETokenType::ESCTwiceDerivedValueToken>;
using ECCTwiceDerivedTagToken = FLEToken<FLETokenType::ECCTwiceDerivedTagToken>;
using ECCTwiceDerivedValueToken = FLEToken<FLETokenType::ECCTwiceDerivedValueToken>;

using ServerTokenDerivationLevel1Token = FLEToken<FLETokenType::ServerTokenDerivationLevel1Token>;
using ServerDerivedFromDataToken = FLEToken<FLETokenType::ServerDerivedFromDataToken>;
using ServerCountAndContentionFactorEncryptionToken =
    FLEToken<FLETokenType::ServerCountAndContentionFactorEncryptionToken>;
using ServerZerosEncryptionToken = FLEToken<FLETokenType::ServerZerosEncryptionToken>;

using AnchorPaddingRootToken = FLEToken<FLETokenType::AnchorPaddingRootToken>;
using AnchorPaddingKeyToken = FLEToken<FLETokenType::AnchorPaddingKeyToken>;
using AnchorPaddingValueToken = FLEToken<FLETokenType::AnchorPaddingValueToken>;

/**
 * Values of ECOC documents in Queryable Encryption protocol version 2
 *
 * Encrypt(ECOCToken, ESCDerivedFromDataTokenAndContentionFactorToken)
 *
 * struct {
 *    uint8_t[32] esc;
 *    uint8_t isLeaf; // Optional: 0 or 1 for range operations, absent for equality.
 * }
 */
class StateCollectionTokensV2 {
public:
    StateCollectionTokensV2() = default;

    /**
     * Initialize ESCTV2 with an unencrypted payload of ESCToken and isLeaf.
     * Must call encrypt() before attempting to serialize.
     */
    StateCollectionTokensV2(ESCDerivedFromDataTokenAndContentionFactorToken s,
                            boost::optional<bool> leaf)
        : _esc(std::move(s)), _isLeaf(std::move(leaf)) {}

    /**
     * Return the ESCDerivedFromDataTokenAndContentionFactorToken value.
     */
    const ESCDerivedFromDataTokenAndContentionFactorToken&
    getESCDerivedFromDataTokenAndContentionFactorToken() const {
        return _esc;
    }

    /**
     * Returns true if the encryptedToken is an equality token, false otherwise.
     */
    bool isEquality() const {
        return getIsLeaf() == boost::none;
    }

    /**
     * Returns true if the encryptedToken is a range token, false otherwise.
     */
    bool isRange() const {
        return getIsLeaf() != boost::none;
    }

    /**
     * Returns the trinary value isLeaf indicating equality(none), range-leaf(true), or
     * range-nonleaf(false).
     */
    boost::optional<bool> getIsLeaf() const {
        return _isLeaf;
    }

    /**
     * StateCollectionTokensV2 serialized into a packed structure and encrypted.
     */
    class Encrypted {
    public:
        Encrypted() = default;
        explicit Encrypted(std::vector<std::uint8_t> encryptedTokens)
            : _encryptedTokens(std::move(encryptedTokens)) {
            assertLength(_encryptedTokens.size());
        }

        static Encrypted parse(ConstDataRange encryptedTokens) {
            std::vector<std::uint8_t> ret;
            const auto* src = encryptedTokens.data<std::uint8_t>();
            std::copy(src, src + encryptedTokens.length(), std::back_inserter(ret));
            return Encrypted(std::move(ret));
        }

        /**
         * Serialize the encrypted payload to a CDR.
         */
        ConstDataRange toCDR() const {
            // Assert length on serialization to ensure we're not serializing a default object.
            assertLength(_encryptedTokens.size());
            return {_encryptedTokens.data(), _encryptedTokens.size()};
        }

        /**
         * Generate a BSON document consisting of:
         * {
         *   "_id": OID::gen(),
         *   "fieldName": {fieldName},
         *   "value": {encryptedTokens},
         * }
         */
        BSONObj generateDocument(StringData fieldName) const;

        /**
         * Decrypt _encryptedTokens back to esc/isLeaf using ECOCToken.
         */
        StateCollectionTokensV2 decrypt(const ECOCToken& token) const;

    private:
        // Encrypted payloads should be 48 or 49 bytes in length.
        // IV(16) + PrfBlock(32) + optional-range-flag(1)
        static constexpr std::size_t kCTRIVSize = 16;
        static constexpr std::size_t kCipherLengthESCOnly = kCTRIVSize + sizeof(PrfBlock);
        static constexpr std::size_t kCipherLengthESCAndLeafFlag = kCipherLengthESCOnly + 1;

        static void assertLength(std::size_t sz) {
            using namespace fmt::literals;
            uassert(
                ErrorCodes::BadValue,
                "Invalid length for EncryptedStateCollectionTokensV2, expected {} or {}, got {}"_format(
                    sizeof(PrfBlock), sizeof(PrfBlock) + 1, sz),
                (sz == kCipherLengthESCOnly) || (sz == kCipherLengthESCAndLeafFlag));
        }

        std::vector<std::uint8_t> _encryptedTokens;
    };

    /**
     * Encrypt _esc/_isLeaf and using ECOCToken.
     */
    Encrypted encrypt(const ECOCToken& token) const;

private:
    ESCDerivedFromDataTokenAndContentionFactorToken _esc;
    boost::optional<bool> _isLeaf;
};

}  // namespace mongo
