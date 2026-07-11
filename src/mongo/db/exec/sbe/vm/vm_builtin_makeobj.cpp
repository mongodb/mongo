// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// NO OTHER HEADERS AFTER THIS LINE
#define MONGO_SBE_VM_MAKEOBJ_H_WHITELIST
#include "mongo/db/exec/sbe/vm/vm_makeobj.h"

namespace mongo::sbe::vm {
value::TagValueMaybeOwned ByteCode::builtinMakeObj(ArityType arity, const CodeFragment* code) {
    tassert(9531000,
            str::stream() << "Unsupported number of args passed to makeObj(): " << arity,
            arity >= 2);

    const int argsStackOff = 2;
    const auto impl = MakeObjImpl{*this, argsStackOff, code};

    return impl.makeObj<ObjectWriter, ArrayWriter>();
}
}  // namespace mongo::sbe::vm
