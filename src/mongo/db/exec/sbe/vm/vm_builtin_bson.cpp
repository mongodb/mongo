/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
        return value::TagValueMaybeOwned::fromRaw(moveFromStack(2));
    } else {
        // Return the input value.
        return value::TagValueMaybeOwned::fromRaw(moveFromStack(0));
    }
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
