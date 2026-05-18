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
        return {false, value::TypeTags::Nothing, 0};
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
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(matches)};
}

value::TagValueMaybeOwned ByteCode::builtinRunJsPredicate(ArityType arity) {
    tassert(11080024, "Unexpected arity value", arity == 2);

    auto predicate = viewFromStack(0);
    auto input = viewFromStack(1);

    if (predicate.tag != value::TypeTags::jsFunction || !value::isObject(input.tag)) {
        return {false, value::TypeTags::Nothing, value::bitcastFrom<int64_t>(0)};
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
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(predicateResult)};
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
        return {false, value::TypeTags::Nothing, 0};
    }

    BSONObj keyAsUnownedBson{sbe::value::bitcastTo<const char*>(shardKey.value)};
    return {false,
            value::TypeTags::Boolean,
            value::bitcastFrom<bool>(
                value::getShardFiltererView(filter.value)->keyBelongsToMe(keyAsUnownedBson))};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
