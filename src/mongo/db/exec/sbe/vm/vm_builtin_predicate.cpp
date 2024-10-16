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
FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinFtsMatch(ArityType arity) {
    invariant(arity == 2);

    auto [matcherOwn, matcherTag, matcherVal] = getFromStack(0);
    auto [inputOwn, inputTag, inputVal] = getFromStack(1);

    if (matcherTag != value::TypeTags::ftsMatcher || !value::isObject(inputTag)) {
        return {false, value::TypeTags::Nothing, 0};
    }

    auto obj = [inputTag = inputTag, inputVal = inputVal]() {
        if (inputTag == value::TypeTags::bsonObject) {
            return BSONObj{value::bitcastTo<const char*>(inputVal)};
        }

        invariant(inputTag == value::TypeTags::Object);
        BSONObjBuilder builder;
        bson::convertToBsonObj(builder, value::getObjectView(inputVal));
        return builder.obj();
    }();

    const bool matches = value::getFtsMatcherView(matcherVal)->matches(obj);
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(matches)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinRunJsPredicate(ArityType arity) {
    invariant(arity == 2);

    auto [predicateOwned, predicateType, predicateValue] = getFromStack(0);
    auto [inputOwned, inputType, inputValue] = getFromStack(1);

    if (predicateType != value::TypeTags::jsFunction || !value::isObject(inputType)) {
        return {false, value::TypeTags::Nothing, value::bitcastFrom<int64_t>(0)};
    }

    BSONObj obj;
    if (inputType == value::TypeTags::Object) {
        BSONObjBuilder objBuilder;
        bson::convertToBsonObj(objBuilder, value::getObjectView(inputValue));
        obj = objBuilder.obj();
    } else if (inputType == value::TypeTags::bsonObject) {
        obj = BSONObj(value::getRawPointerView(inputValue));
    } else {
        MONGO_UNREACHABLE;
    }

    auto predicate = value::getJsFunctionView(predicateValue);
    auto predicateResult = predicate->runAsPredicate(obj);
    return {false, value::TypeTags::Boolean, value::bitcastFrom<bool>(predicateResult)};
}

FastTuple<bool, value::TypeTags, value::Value> ByteCode::builtinShardFilter(ArityType arity) {
    invariant(arity == 2);

    auto [ownedFilter, filterTag, filterValue] = getFromStack(0);
    auto [ownedShardKey, shardKeyTag, shardKeyValue] = getFromStack(1);

    if (filterTag != value::TypeTags::shardFilterer || shardKeyTag != value::TypeTags::bsonObject) {
        if (filterTag == value::TypeTags::shardFilterer &&
            shardKeyTag == value::TypeTags::Nothing) {
            LOGV2_WARNING(5071200,
                          "No shard key found in document, it may have been inserted manually "
                          "into shard",
                          "keyPattern"_attr =
                              value::getShardFiltererView(filterValue)->getKeyPattern());
        }
        return {false, value::TypeTags::Nothing, 0};
    }

    BSONObj keyAsUnownedBson{sbe::value::bitcastTo<const char*>(shardKeyValue)};
    return {false,
            value::TypeTags::Boolean,
            value::bitcastFrom<bool>(
                value::getShardFiltererView(filterValue)->keyBelongsToMe(keyAsUnownedBson))};
}

}  // namespace vm
}  // namespace sbe
}  // namespace mongo
