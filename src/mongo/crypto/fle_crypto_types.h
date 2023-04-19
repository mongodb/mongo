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
#include <cstdint>
#include <vector>

#include "mongo/base/secure_allocator.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/util/uuid.h"

namespace mongo {

constexpr auto kSafeContent = "__safeContent__"_sd;
constexpr auto kSafeContentString = "__safeContent__";

using PrfBlock = std::array<std::uint8_t, 32>;
using KeyMaterial = SecureVector<std::uint8_t>;

// u = [1, max parallel clients)
using FLEContentionFactor = std::uint64_t;
using FLECounter = std::uint64_t;


/**
 * There are two types of keys that are user supplied.
 * 1. Index, aka S - this encrypts the index structures
 * 2. User, aka K - this encrypts the user data.
 *
 * These keys only exist on the client, they are never on the server-side.
 */
enum class FLEKeyType {
    Index,  // i.e. S
    User,   // i.e. K
};

/**
 * Template class to ensure unique C++ types for each key.
 */
template <FLEKeyType KeyT>
struct FLEKey {
    FLEKey() = default;

    FLEKey(KeyMaterial dataIn) : data(std::move(dataIn)) {
        // This is not a mistake; same keys will be used in FLE2 as in FLE1
        uassert(6364500,
                str::stream() << "Length of KeyMaterial is expected to be "
                              << crypto::kFieldLevelEncryptionKeySize << " bytes, found "
                              << data->size(),
                data->size() == crypto::kFieldLevelEncryptionKeySize);
    }

    ConstDataRange toCDR() const {
        return ConstDataRange(data->data(), data->data() + data->size());
    }

    // Actual type of the key
    FLEKeyType type{KeyT};

    // Raw bytes of the key
    KeyMaterial data;
};

using FLEIndexKey = FLEKey<FLEKeyType::Index>;
using FLEUserKey = FLEKey<FLEKeyType::User>;

/**
 * Key Material and its UUID id.
 *
 * The UUID is persisted into the serialized structures so that decryption is self-describing.
 */
template <FLEKeyType KeyT>
struct FLEKeyAndId {

    FLEKeyAndId(KeyMaterial material, UUID uuid) : key(material), keyId(uuid) {}

    FLEKey<KeyT> key;
    UUID keyId;
};

using FLEIndexKeyAndId = FLEKeyAndId<FLEKeyType::Index>;
using FLEUserKeyAndId = FLEKeyAndId<FLEKeyType::User>;


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
 * EDCToken = HMAC(CollectionsLevel1Token, 1) = K^{edc}_f
 * ESCToken = HMAC(CollectionsLevel1Token, 2) = K^{esc}_f
 * ECCToken = HMAC(CollectionsLevel1Token, 3) = K^{ecc}_f
 * ECOCToken = HMAC(CollectionsLevel1Token, 4) = K^{ecoc}_f = Fs[f,1,4]
 *
 * EDCDerivedFromDataToken = HMAC(EDCToken, v) = K^{edc}_{f,v} = Fs[f,1,1,v]
 * ESCDerivedFromDataToken = HMAC(ESCToken, v) = K^{esc}_{f,v} = Fs[f,1,2,v]
 * ECCDerivedFromDataToken = HMAC(ECCToken, v) = K^{ecc}_{f,v} = Fs[f,1,3,v]
 *
 * EDCDerivedFromDataTokenAndContentionFactorToken = HMAC(EDCDerivedFromDataToken, u) =
 * Fs[f,1,1,v,u] ESCDerivedFromDataTokenAndContentionFactorToken = HMAC(ESCDerivedFromDataToken, u)
 * = Fs[f,1,2,v,u] ECCDerivedFromDataTokenAndContentionFactorToken = HMAC(ECCDerivedFromDataToken,
 * u) = Fs[f,1,3,v,u]
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
};

/**
 * Templated C++ class that contains a token. A templated class is used to create a strongly typed
 * API that is hard to misuse.
 */
template <FLETokenType TokenT>
struct FLEToken {
    FLEToken() = default;

    FLEToken(PrfBlock dataIn) : data(std::move(dataIn)) {}

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

/**
 * A pair of a (ESCDerivedFromDataTokenAndContentionFactorToken, optional
 * EDCDerivedFromDataTokenAndContentionFactorToken) that will be used to lookup a count for the ESC
 * token from ESC. The EDC token is simply passed through to the response for query tag generation.
 * The inclusion of EDC simplifies the code that processes the response.
 */
struct FLEEdgePrfBlock {
    PrfBlock esc;                   // ESCDerivedFromDataTokenAndContentionFactorToken
    boost::optional<PrfBlock> edc;  // EDCDerivedFromDataTokenAndContentionFactorToken
};

/**
 * A pair of non-anchor and anchor positions.
 */
struct ESCCountsPair {
    uint64_t cpos;
    uint64_t apos;
};

/**
 * A pair of optional non-anchor and anchor positions returned by emulated binary search.
 */
struct EmuBinaryResult {
    boost::optional<uint64_t> cpos;
    boost::optional<uint64_t> apos;
};

/**
 * The information retrieved from ESC for a given ESC token. Count may reflect a count suitable for
 * insert or query.
 */
struct FLEEdgeCountInfo {
    FLEEdgeCountInfo(uint64_t c, ESCTwiceDerivedTagToken t) : count(c), tagToken(t) {}

    FLEEdgeCountInfo(uint64_t c,
                     ESCTwiceDerivedTagToken t,
                     boost::optional<EDCDerivedFromDataTokenAndContentionFactorToken> edcParam)
        : count(c), tagToken(t), edc(edcParam) {}

    FLEEdgeCountInfo(uint64_t c,
                     ESCTwiceDerivedTagToken t,
                     boost::optional<EmuBinaryResult> searchedCounts,
                     boost::optional<ESCCountsPair> nullAnchorCounts,
                     boost::optional<ECStats> stats,
                     boost::optional<EDCDerivedFromDataTokenAndContentionFactorToken> edcParam)
        : count(c),
          tagToken(t),
          searchedCounts(searchedCounts),
          nullAnchorCounts(nullAnchorCounts),
          stats(stats),
          edc(edcParam) {}


    // May reflect a value suitable for insert or query.
    uint64_t count;

    ESCTwiceDerivedTagToken tagToken;

    // Positions returned by emuBinary (used by compact & cleanup)
    boost::optional<EmuBinaryResult> searchedCounts;

    // Positions obtained from null anchor decode (used by cleanup)
    boost::optional<ESCCountsPair> nullAnchorCounts;

    boost::optional<ECStats> stats;

    boost::optional<EDCDerivedFromDataTokenAndContentionFactorToken> edc;
};

}  // namespace mongo
