// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/vm/vm.h"

namespace mongo {
namespace sbe {
namespace vm {
value::TagValueMaybeOwned ByteCode::builtinCoerceToBool(ArityType arity) {
    auto operand = viewFromStack(0);

    auto [tag, val] = value::coerceToBool(operand.tag, operand.value);

    return {false, tag, val};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
