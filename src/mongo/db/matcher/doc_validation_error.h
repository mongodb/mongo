/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/error_extra_info.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/query_knobs_gen.h"

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
    explicit DocumentValidationFailureInfo(const BSONObj& err) : _details(err.getOwned()) {}
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