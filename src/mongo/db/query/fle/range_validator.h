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

#include "mongo/bson/util/builder_fwd.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/util/assert_util.h"

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
void validateRanges(const MatchExpression& expr);
void validateRanges(const Expression& expr);
}  // namespace fle
}  // namespace mongo
