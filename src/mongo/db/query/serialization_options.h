/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once
#include "mongo/base/string_data.h"
#include "mongo/util/assert_util.h"
#include <boost/optional.hpp>
#include <string>

namespace mongo {
namespace {
// Should never be called, throw to ensure we catch this in tests.
std::string defaultRedactionStrategy(StringData s) {
    MONGO_UNREACHABLE_TASSERT(7332410);
}
}  // namespace

/**
 * A struct with options for how you want to serialize a match or aggregation expression.
 */
struct SerializationOptions {
    SerializationOptions() {}

    SerializationOptions(bool explain_) : explain(explain_) {}

    SerializationOptions(std::function<std::string(StringData)> redactFieldNamesStrategy_,
                         boost::optional<StringData> replacementForLiteralArgs_)
        : replacementForLiteralArgs(replacementForLiteralArgs_),
          redactFieldNames(redactFieldNamesStrategy_),
          redactFieldNamesStrategy(redactFieldNamesStrategy_) {}

    std::string serializeFieldName(StringData str) const {
        if (redactFieldNames) {
            return redactFieldNamesStrategy(str);
        }
        return str.toString();
    }

    // 'replacementForLiteralArgs' is an independent option to serialize in a genericized format
    // with the aim of similar "shaped" queries serializing to the same object. For example, if
    // set to '?' then the serialization of {a: {$gt: 2}} will result in {a: {$gt: '?'}}, as
    // will the serialization of {a: {$gt: 3}}.
    //
    // "Literal" here is meant to stand in contrast to expression arguements, as in the $gt
    // expressions in {$and: [{a: {$gt: 3}}, {b: {$gt: 4}}]}. There the only literals are 3 and
    // 4, so the serialization expected would be {$and: [{a: {$gt: '?'}}, {b: {$lt: '?'}}]}.
    boost::optional<StringData> replacementForLiteralArgs = boost::none;

    // If true the caller must set redactFieldNamesStrategy. 'redactFieldNames' if set along with
    // a strategy the redaction strategy will be called on any field paths/names encountered
    // before serializing them.
    bool redactFieldNames = false;
    std::function<std::string(StringData)> redactFieldNamesStrategy = defaultRedactionStrategy;

    // If set, serializes without including the path. For example {a: {$gt: 2}} would serialize
    // as just {$gt: 2}.
    //
    // It is expected that most callers want to set 'includePath' to true to
    // get a correct serialization. Internally, we may set this to false if we have a situation
    // where an outer expression serializes a path and we don't want to repeat the path in the
    // inner expression.
    //
    // For example in {a: {$elemMatch: {$eq: 2}}} the "a" is serialized by the $elemMatch, and
    // should not be serialized by the EQ child.
    // The $elemMatch will serialize {a: {$elemMatch: <recurse>}} and the EQ will serialize just
    // {$eq: 2} instead of its usual {a: {$eq: 2}}.
    bool includePath = true;

    // For aggregation indicate whether we should use the more verbose serialization format.
    bool explain = false;
};

}  // namespace mongo
