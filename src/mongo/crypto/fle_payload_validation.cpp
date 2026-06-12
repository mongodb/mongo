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

#include "mongo/crypto/fle_payload_validation.h"

#include "mongo/crypto/encryption_fields_util.h"
#include "mongo/crypto/encryption_fields_validation.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string>

namespace mongo {
namespace {
// Like hasQueryType, but a payload's token variant maps to the GA query type (Suffix/Prefix) while
// a field created before the GA types existed carries the deprecated preview variant. Accept
// either: getAndValidateSchema blocks deprecated schemas once featureFlagQEPrefixSuffixSearch is
// on, so a deprecated type only reaches here while the field is still legitimately operational.
bool hasQueryTypeOrPreview(const EncryptedField& field, QueryTypeEnum t) {
    return hasQueryTypeMatching(field, [&](QueryTypeEnum qt) {
        if (qt == t) {
            return true;
        }
        if (t == QueryTypeEnum::Suffix) {
            return qt == QueryTypeEnum::SuffixPreviewDeprecated;
        }
        if (t == QueryTypeEnum::Prefix) {
            return qt == QueryTypeEnum::PrefixPreviewDeprecated;
        }
        return false;
    });
}
}  // namespace

FLE2PayloadParams::FLE2PayloadParams(const ParsedFindEqualityPayload& p) {
    contentionKind = FLE2PayloadParams::ContentionKind::kConfiguredMax;
    contention = p.maxCounter;
    expectedTypes = {QueryTypeEnum::Equality};
}

FLE2PayloadParams::FLE2PayloadParams(const ParsedFindRangePayload& p) {
    contentionKind = FLE2PayloadParams::ContentionKind::kConfiguredMax;
    sparsity = p.sparsity.map([](std::int32_t v) { return static_cast<std::int64_t>(v); });
    precision = p.precision;
    // Stubs (the no-edges half of a two-sided range) carry no maxCounter.
    if (p.edges) {
        contention = p.maxCounter;
    }
    if (p.indexMin) {
        indexMin = p.indexMin->getElement();
    }
    if (p.indexMax) {
        indexMax = p.indexMax->getElement();
    }
    expectedTypes = {QueryTypeEnum::Range};
}

FLE2PayloadParams::FLE2PayloadParams(const ParsedFindTextSearchPayload& p) {
    contentionKind = FLE2PayloadParams::ContentionKind::kConfiguredMax;
    contention = p.maxCounter;
    if (p.prefixTokens) {
        expectedTypes = {QueryTypeEnum::Prefix};
    } else if (p.suffixTokens) {
        expectedTypes = {QueryTypeEnum::Suffix};
    } else if (p.substringTokens) {
        expectedTypes = {QueryTypeEnum::SubstringPreview};
    } else {
        // $encStrNormalizedEq (exact tokens) has no dedicated QueryType; EFC validation enforces
        // that contention, caseSensitive, and diacriticSensitive are identical across every text
        // QTC on the field, so any text QTC is acceptable.
        matchAnyStringSearchType = true;
    }
}

void validatePayloadAgainstQueryTypeConfig(StringData fieldPath,
                                           const EncryptedField& field,
                                           const FLE2PayloadParams& params) {
    QueryTypeConfig qtc;
    if (params.matchAnyStringSearchType) {
        auto stringQueryType = findStringSearchQueryType(field);
        uassert(9188704,
                str::stream()
                    << "Encrypted payload for field '" << fieldPath
                    << "' was generated for normalized string equality search but the field is not "
                       "indexed for any string search query type",
                stringQueryType);
        qtc = getQueryType(field, *stringQueryType);
    } else {
        tassert(9188708, "expectedTypes must be non-empty", !params.expectedTypes.empty());
        for (auto expectedType : params.expectedTypes) {
            uassert(9188707,
                    str::stream() << "Encrypted payload for field '" << fieldPath
                                  << "' was generated for query type '"
                                  << idl::serialize(expectedType)
                                  << "' but the field is not indexed for that query type",
                    hasQueryTypeOrPreview(field, expectedType));
        }
        // Contention/sparsity/precision/min/max are shared across a field's QTCs (EFC validation
        // enforces this), so reading the first expected type's config is sufficient. For text types
        // the field may carry the deprecated preview variant, so resolve via
        // findStringSearchQueryType (guaranteed non-none here: the loop above asserted the field
        // has the text type).
        auto front = params.expectedTypes.front();
        if (isFLE2TextQueryType(front)) {
            auto configuredStringQuery = findStringSearchQueryType(field);
            tassert(9188710,
                    "field validated for a text query type must have a text QueryTypeConfig",
                    configuredStringQuery);
            qtc = getQueryType(field, *configuredStringQuery);
        } else {
            qtc = getQueryType(field, front);
        }
    }
    const auto configContention = qtc.getContention();
    if (params.contention) {
        if (params.contentionKind == FLE2PayloadParams::ContentionKind::kSampled) {
            uassert(9188700,
                    str::stream() << "contentionFactor " << *params.contention
                                  << " in payload for field '" << fieldPath
                                  << "' exceeds collection's configured contention "
                                  << configContention,
                    *params.contention <= configContention);
        } else {
            uassert(9188701,
                    str::stream() << "max contention " << *params.contention
                                  << " in find payload for field '" << fieldPath
                                  << "' does not match collection's configured contention "
                                  << configContention,
                    *params.contention == configContention);
        }
    }

    if (params.sparsity) {
        const auto configSparsity = qtc.getSparsity().value_or(kFLERangeSparsityDefault);
        uassert(9188702,
                str::stream() << "sparsity " << *params.sparsity << " in payload for field '"
                              << fieldPath << "' does not match collection's configured sparsity "
                              << configSparsity,
                *params.sparsity == configSparsity);
    }

    if (params.precision) {
        uassert(
            9188703,
            str::stream() << "precision " << *params.precision << " in payload for field '"
                          << fieldPath << "' does not match collection's configured precision "
                          << (qtc.getPrecision() ? std::to_string(*qtc.getPrecision()) : "(none)"),
            qtc.getPrecision() && *params.precision == *qtc.getPrecision());
    }

    // TODO (SERVER-127899): validate trimFactor once payload/config trimFactor semantics are
    // aligned.

    // Value equality matches the encryption layer's numeric-type normalization (Int/Long/Double
    // with the same value produce the same OST tokens).
    if (params.indexMin && qtc.getMin()) {
        uassert(9188705,
                str::stream() << "indexMin in payload for field '" << fieldPath
                              << "' does not match collection's configured indexMin",
                Value::compare(Value{*params.indexMin}, *qtc.getMin(), nullptr) == 0);
    }
    if (params.indexMax && qtc.getMax()) {
        uassert(9188706,
                str::stream() << "indexMax in payload for field '" << fieldPath
                              << "' does not match collection's configured indexMax",
                Value::compare(Value{*params.indexMax}, *qtc.getMax(), nullptr) == 0);
    }
}

}  // namespace mongo
