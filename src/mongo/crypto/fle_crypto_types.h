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
#include "mongo/crypto/fle_tokens.h"
#include "mongo/util/uuid.h"

namespace mongo {

constexpr auto kSafeContent = "__safeContent__"_sd;
constexpr auto kSafeContentString = "__safeContent__";

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
 * The information retrieved from ESC for a given ESC token or anchor padding token. Count may
 * reflect a count suitable for insert or query.
 */
struct FLEEdgeCountInfo {
    FLEEdgeCountInfo(uint64_t c, PrfBlock tData) : count(c), tagTokenData(tData) {}

    FLEEdgeCountInfo(uint64_t c,
                     PrfBlock tData,
                     boost::optional<EDCDerivedFromDataTokenAndContentionFactorToken> edcParam)
        : count(c), tagTokenData(tData), edc(edcParam) {}

    FLEEdgeCountInfo(uint64_t c,
                     PrfBlock tData,
                     boost::optional<EmuBinaryResult> searchedCounts,
                     boost::optional<ESCCountsPair> nullAnchorCounts,
                     boost::optional<ECStats> stats,
                     boost::optional<EDCDerivedFromDataTokenAndContentionFactorToken> edcParam)
        : count(c),
          tagTokenData(tData),
          searchedCounts(searchedCounts),
          nullAnchorCounts(nullAnchorCounts),
          stats(stats),
          edc(edcParam) {}


    // May reflect a value suitable for insert or query.
    uint64_t count;

    PrfBlock tagTokenData;

    // Positions returned by emuBinary (used by compact & cleanup)
    boost::optional<EmuBinaryResult> searchedCounts;

    // Positions obtained from null anchor decode (used by cleanup)
    boost::optional<ESCCountsPair> nullAnchorCounts;

    boost::optional<ECStats> stats;

    boost::optional<EDCDerivedFromDataTokenAndContentionFactorToken> edc;
};

}  // namespace mongo
