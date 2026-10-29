// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/optimizer/join/join_predicate.h"

#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
std::string opToString(QSNJoinPredicate::ComparisonOp op) {
    switch (op) {
        case QSNJoinPredicate::ComparisonOp::Eq:
            return "=";
        case QSNJoinPredicate::ComparisonOp::ExprEq:
            return "$=";
    }
    MONGO_UNREACHABLE_TASSERT(11083901);
}
}  // namespace

std::string QSNJoinPredicate::toString() const {
    return str::stream() << leftField.fullPath() << " " << opToString(op) << " "
                         << rightField.fullPath();
}
}  // namespace mongo
