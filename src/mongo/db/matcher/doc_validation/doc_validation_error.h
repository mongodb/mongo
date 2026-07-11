// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>

[[MONGO_MOD_PUBLIC]];
namespace mongo::doc_validation_error {
// The default maximum allowed size for a single doc validation error.
constexpr static int kDefaultMaxDocValidationErrorSize = 12 * 1024 * 1024;

/**
 * Represents information about a document validation error.
 */
class DocumentValidationFailureInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::DocumentValidationFailure;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);
    explicit DocumentValidationFailureInfo(const BSONObj& err) : _details(err.getOwned()) {
        tassert(9740340,
                "Cannot construct 'DocumentValidationFailureInfo' with non-empty error",
                !err.isEmpty());
    }
    const BSONObj& getDetails() const;
    void serialize(BSONObjBuilder* bob) const override;

private:
    BSONObj _details;
};

/**
 * Given a pointer to a MatchExpression corresponding to a collection's validator expression and a
 * reference to a BSONObj corresponding to the document that failed to match against the validator
 * expression, returns a BSONObj that describes why 'doc' failed to match against 'validatorExpr'.
 */
BSONObj generateError(
    const MatchExpression& validatorExpr,
    const BSONObj& doc,
    int maxDocValidationErrorSize = kDefaultMaxDocValidationErrorSize,
    int maxConsideredValues = internalQueryMaxDocValidationErrorConsideredValues.load());
}  // namespace mongo::doc_validation_error
