// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/document_comparator.h"

#include "mongo/util/assert_util.h"

namespace mongo {

bool DocumentComparator::evaluate(Document::DeferredComparison deferredComparison) const {
    int cmp = Document::compare(deferredComparison.lhs, deferredComparison.rhs, _stringComparator);
    switch (deferredComparison.type) {
        case Document::DeferredComparison::Type::kLT:
            return cmp < 0;
        case Document::DeferredComparison::Type::kLTE:
            return cmp <= 0;
        case Document::DeferredComparison::Type::kEQ:
            return cmp == 0;
        case Document::DeferredComparison::Type::kGTE:
            return cmp >= 0;
        case Document::DeferredComparison::Type::kGT:
            return cmp > 0;
        case Document::DeferredComparison::Type::kNE:
            return cmp != 0;
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
