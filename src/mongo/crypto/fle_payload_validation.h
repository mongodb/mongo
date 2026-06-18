/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonelement.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"

#include <cstdint>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Payload-side parameters to validate against a QueryTypeConfig. Fields the payload didn't carry
 * are left as boost::none and skipped. `expectedTypes` are the QueryTypes the payload was generated
 * for (a payload may carry several, e.g. an insert for a field configured with both prefix and
 * suffix); the validator throws unless the field is configured for every one of them. A find
 * payload for a normalized string equality search may be encountered for fields having any of the
 * string search query types, so `matchAnyStringSearchType` lets `expectedTypes` be empty but
 * requires the field to have a string search query type.
 * NOTE: The user of an FLE2PayloadParams struct must ensure the backing BSON for the payload stays
 * alive while the FLE2PayloadParams struct is in use.
 */
struct FLE2PayloadParams {
    /**
     * Contention semantics in an FLE2 payload: insert/update carries a sampled k in [0,
     * contention]; range-find carries the configured max (cm == contention).
     */
    enum class ContentionKind {
        kSampled,
        kConfiguredMax,
    };

    boost::optional<std::int64_t> contention;
    ContentionKind contentionKind = ContentionKind::kConfiguredMax;
    boost::optional<std::int64_t> sparsity;
    boost::optional<std::int32_t> precision;
    boost::optional<BSONElement> indexMin;
    boost::optional<BSONElement> indexMax;
    std::vector<QueryTypeEnum> expectedTypes;
    bool matchAnyStringSearchType = false;

    FLE2PayloadParams() = default;
    explicit FLE2PayloadParams(const ParsedFindEqualityPayload& p);
    explicit FLE2PayloadParams(const ParsedFindRangePayload& p);
    explicit FLE2PayloadParams(const ParsedFindTextSearchPayload& p);
};

// Check that the payload's params agree with the QueryTypeConfig(s) at `params.expectedTypes` on
// `field`. Throws if `field` is not configured for every expected type.
void validatePayloadAgainstQueryTypeConfig(std::string_view fieldPath,
                                           const EncryptedField& field,
                                           const FLE2PayloadParams& params);

// Insert/update payload -> FLE2PayloadParams. kSampled: contention is the bucket the client chose.
// The expected query types are derived from the token sets the payload carries.
inline FLE2PayloadParams toFLE2PayloadParams(const FLE2InsertUpdatePayloadV2& iup) {
    FLE2PayloadParams r;
    r.contentionKind = FLE2PayloadParams::ContentionKind::kSampled;
    r.contention = iup.getContentionFactor();
    r.sparsity = iup.getSparsity();
    r.precision = iup.getPrecision();
    if (iup.getIndexMin()) {
        r.indexMin = iup.getIndexMin()->getElement();
    }
    if (iup.getIndexMax()) {
        r.indexMax = iup.getIndexMax()->getElement();
    }
    if (iup.getEdgeTokenSet()) {
        r.expectedTypes.push_back(QueryTypeEnum::Range);
    } else if (const auto& tsts = iup.getTextSearchTokenSets(); tsts.has_value()) {
        if (!tsts->getSubstringTokenSets().empty()) {
            r.expectedTypes.push_back(QueryTypeEnum::SubstringPreview);
        }
        if (!tsts->getSuffixTokenSets().empty()) {
            r.expectedTypes.push_back(QueryTypeEnum::Suffix);
        }
        if (!tsts->getPrefixTokenSets().empty()) {
            r.expectedTypes.push_back(QueryTypeEnum::Prefix);
        }
    } else {
        r.expectedTypes.push_back(QueryTypeEnum::Equality);
    }
    return r;
}

}  // namespace mongo
