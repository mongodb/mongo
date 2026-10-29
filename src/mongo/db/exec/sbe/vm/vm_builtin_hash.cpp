// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/hasher.h"

namespace mongo {
namespace sbe {
namespace vm {
using namespace std::literals::string_view_literals;
value::TagValueMaybeOwned ByteCode::builtinHash(ArityType arity) {
    auto hashVal = value::hashInit();
    for (ArityType idx = 0; idx < arity; ++idx) {
        auto kv = viewFromStack(idx);
        hashVal = value::hashCombine(hashVal, value::hashValue(kv.tag, kv.value));
    }

    return {false, value::TypeTags::NumberInt64, value::bitcastFrom<decltype(hashVal)>(hashVal)};
}

value::TagValueMaybeOwned ByteCode::builtinShardHash(ArityType arity) {
    tassert(11080027, "Unexpected arity value", arity == 1);

    auto shardKey = viewFromStack(0);

    // Compute the shard key hash value by round-tripping it through BSONObj as it is currently the
    // only way to do it if we do not want to duplicate the hash computation code.
    // TODO SERVER-55622
    BSONObjBuilder input;
    bson::appendValueToBsonObj<BSONObjBuilder>(input, ""sv, shardKey.tag, shardKey.value);
    auto hashVal =
        BSONElementHasher::hash64(input.obj().firstElement(), BSONElementHasher::DEFAULT_HASH_SEED);
    return {false, value::TypeTags::NumberInt64, value::bitcastFrom<decltype(hashVal)>(hashVal)};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
