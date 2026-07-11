// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

// NO OTHER HEADERS AFTER THIS LINE
#define MONGO_SBE_VM_MAKEOBJ_H_WHITELIST
#include "mongo/db/exec/sbe/vm/vm_makeobj.h"

namespace mongo::sbe::vm {
value::TagValueMaybeOwned ByteCode::builtinMakeBsonObj(ArityType arity, const CodeFragment* code) {
    tassert(6897002,
            str::stream() << "Unsupported number of args passed to makeBsonObj(): " << arity,
            arity >= 2);

    const int argsStackOff = 2;
    const auto impl = MakeObjImpl{*this, argsStackOff, code};

    return impl.makeObj<BsonObjWriter, BsonArrWriter>();
}
}  // namespace mongo::sbe::vm
