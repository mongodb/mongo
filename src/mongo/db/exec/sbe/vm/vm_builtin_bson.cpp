// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
value::TagValueMaybeOwned ByteCode::builtinTypeMatch(ArityType arity) {
    tassert(11080053, "Unexpected arity value", arity == 2);

    auto input = viewFromStack(0);
    auto typeMask = viewFromStack(1);

    if (input.tag != value::TypeTags::Nothing && typeMask.tag == value::TypeTags::NumberInt32) {
        auto typeMaskVal = static_cast<uint32_t>(value::bitcastTo<int32_t>(typeMask.value));
        bool matches = static_cast<bool>(getBSONTypeMask(input.tag) & typeMaskVal);

        return value::TagValueMaybeOwned::boolean(matches);
    }

    return value::TagValueMaybeOwned::nothing();
}

value::TagValueMaybeOwned ByteCode::builtinFillType(ArityType arity) {
    tassert(11080052, "Unexpected arity value", arity == 3);

    auto input = viewFromStack(0);
    auto typeMaskView = viewFromStack(1);

    if (typeMaskView.tag != value::TypeTags::NumberInt32 || input.tag == value::TypeTags::Nothing) {
        return {true, value::TypeTags::Nothing, value::Value{0u}};
    }
    uint32_t typeMask = static_cast<uint32_t>(value::bitcastTo<int32_t>(typeMaskView.value));

    if (static_cast<bool>(getBSONTypeMask(input.tag) & typeMask)) {
        // Return the fill value.
        return moveMaybeOwnedFromStack(2);
    } else {
        // Return the input value.
        return moveMaybeOwnedFromStack(0);
    }
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
