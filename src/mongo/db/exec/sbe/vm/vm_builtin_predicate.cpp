// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/js_function.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/vm/vm.h"
#include "mongo/db/fts/fts_matcher.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace sbe {
namespace vm {
value::TagValueMaybeOwned ByteCode::builtinFtsMatch(ArityType arity) {
    tassert(11080025, "Unexpected arity value", arity == 2);

    auto matcher = viewFromStack(0);
    auto input = viewFromStack(1);

    if (matcher.tag != value::TypeTags::ftsMatcher || !value::isObject(input.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    auto obj = [inputTag = input.tag, inputVal = input.value]() {
        if (inputTag == value::TypeTags::bsonObject) {
            return BSONObj{value::bitcastTo<const char*>(inputVal)};
        }

        tassert(
            11086806, "Unexpected type of input parameter", inputTag == value::TypeTags::Object);
        BSONObjBuilder builder;
        bson::convertToBsonObj(builder, value::getObjectView(inputVal));
        return builder.obj();
    }();

    const bool matches = value::getFtsMatcherView(matcher.value)->matches(obj);
    return value::TagValueMaybeOwned::boolean(matches);
}

value::TagValueMaybeOwned ByteCode::builtinRunJsPredicate(ArityType arity) {
    tassert(11080024, "Unexpected arity value", arity == 2);

    auto predicate = viewFromStack(0);
    auto input = viewFromStack(1);

    if (predicate.tag != value::TypeTags::jsFunction || !value::isObject(input.tag)) {
        return value::TagValueMaybeOwned::nothing();
    }

    BSONObj obj;
    if (input.tag == value::TypeTags::Object) {
        BSONObjBuilder objBuilder;
        bson::convertToBsonObj(objBuilder, value::getObjectView(input.value));
        obj = objBuilder.obj();
    } else if (input.tag == value::TypeTags::bsonObject) {
        obj = BSONObj(value::getRawPointerView(input.value));
    } else {
        MONGO_UNREACHABLE_TASSERT(11122945);
    }

    auto jsFn = value::getJsFunctionView(predicate.value);
    auto predicateResult = jsFn->runAsPredicate(obj);
    return value::TagValueMaybeOwned::boolean(predicateResult);
}

value::TagValueMaybeOwned ByteCode::builtinShardFilter(ArityType arity) {
    tassert(11080023, "Unexpected arity value", arity == 2);

    auto filter = viewFromStack(0);
    auto shardKey = viewFromStack(1);

    if (filter.tag != value::TypeTags::shardFilterer ||
        shardKey.tag != value::TypeTags::bsonObject) {
        if (filter.tag == value::TypeTags::shardFilterer &&
            shardKey.tag == value::TypeTags::Nothing) {
            LOGV2_WARNING(5071200,
                          "No shard key found in document, it may have been inserted manually "
                          "into shard",
                          "keyPattern"_attr =
                              value::getShardFiltererView(filter.value)->getKeyPattern());
        }
        return value::TagValueMaybeOwned::nothing();
    }

    BSONObj keyAsUnownedBson{sbe::value::bitcastTo<const char*>(shardKey.value)};
    return value::TagValueMaybeOwned::boolean(
        value::getShardFiltererView(filter.value)->keyBelongsToMe(keyAsUnownedBson));
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
