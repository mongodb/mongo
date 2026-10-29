// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/sbe/vm/vm.h"

#include <cstdint>

namespace mongo::sbe::vm {

int32_t ByteCode::mqlComparisonRank(value::TypeTags tag) {
    switch (tag) {
        case value::TypeTags::MinKey:
            return 0;
        case value::TypeTags::Nothing:
        case value::TypeTags::bsonUndefined:
            return 1;
        default:
            return 2;
    }
}

}  // namespace mongo::sbe::vm
