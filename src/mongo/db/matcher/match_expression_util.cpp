// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/match_expression_util.h"

namespace mongo::match_expression_util {
void advanceBy(size_t numberOfElements, BSONObjIterator& iterator) {
    for (size_t i = 0; iterator.more() && i < numberOfElements; ++i) {
        iterator.next();
    }
}
}  // namespace mongo::match_expression_util
