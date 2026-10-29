// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/util/builder_fwd.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo {
// This overload is used when building validation error messages.
inline StringBuilder& operator<<(StringBuilder& os, const Fle2RangeOperator& op) {
    switch (op) {
        case Fle2RangeOperator::kGt:
            return os << "$gt";
        case Fle2RangeOperator::kGte:
            return os << "$gte";
        case Fle2RangeOperator::kLt:
            return os << "$lt";
        case Fle2RangeOperator::kLte:
            return os << "$lte";
    }
    MONGO_UNREACHABLE_TASSERT(7030719);
}
namespace fle {
/**
 * Encrypted range predicates are transmitted from the client to the server in $gt and $lt
 * operators. Since an encrypted payload is not divisible in to two separate BinData blobs where
 * only one operator actually contains the encrypted payload, while the other one acts as a stub.
 *
 * Users have the option of bypassing query analysis, generating encrypted payloads manually, and
 * manually constructing queries. This can lead to a situation where the user has generated payloads
 * for the query {x: {$gt: 23, $lte: 35}} => {x: {$gt: <payload for (23, 35]>, $lte: <stub>}}, but
 * then modifies the query so that {x: {$gte: <payload for (23, 35]>, $lte: <stub>}} is sent to the
 * server. Since the payloads are semantically opaque to the server, the results will be aligned
 * with the original query with $gt/$lte bounds, not the modified query with $gte/$lte bounds.
 *
 * The BinData blobs are generated with unique payloadIds that match a payload and stub together, as
 * well as indicate what MQL operator the blob was generated for. This validation pass uses that
 * information to ensure that payloads and stubs are properly matched with comparison operators so
 * that the visible syntax of a query matches with its encrypted semantics. In the case of a
 * mismatch, the server will return an error reminding the user to regenerate their encrypted
 * payloads for their new query.
 */
void validateRanges(const MatchExpression& expr, boost::optional<const EncryptedFieldConfig&> efc);
void validateRanges(const Expression& expr, boost::optional<const EncryptedFieldConfig&> efc);
}  // namespace fle
}  // namespace mongo
