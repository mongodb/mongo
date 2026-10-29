// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/secure_allocator.h"
#include "mongo/crypto/aead_encryption.h"
#include "mongo/crypto/fle_key_types.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/crypto/fle_tokens.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <array>
#include <cstdint>
#include <vector>

#include <fmt/format.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {
using namespace std::literals::string_view_literals;

constexpr auto kSafeContent = "__safeContent__"sv;
constexpr auto kSafeContentString = "__safeContent__";

// u = [1, max parallel clients)
using FLEContentionFactor = std::uint64_t;
using FLECounter = std::uint64_t;

/**
 * A pair of a (ESCDerivedFromDataTokenAndContentionFactorToken, optional
 * EDCDerivedFromDataTokenAndContentionFactorToken) that will be used to lookup a count for the ESC
 * token from ESC. The EDC token is simply passed through to the response for query tag generation.
 * The inclusion of EDC simplifies the code that processes the response.
 */
struct FLEEdgePrfBlock {
    PrfBlock esc;                   // ESCDerivedFromDataTokenAndContentionFactorToken
    boost::optional<PrfBlock> edc;  // EDCDerivedFromDataTokenAndContentionFactorToken

    // Text search tokens sets may contain multiple identical "padding" esc & edc tokens.
    // This zero-based counter can be used to number & disambiguate those padding tokens.
    uint32_t paddingIndex = 0;
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
