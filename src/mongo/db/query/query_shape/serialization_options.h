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
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
// Should never be called, throw to ensure we catch this in tests.
std::string defaultHmacStrategy(StringData s) {
    MONGO_UNREACHABLE_TASSERT(7332410);
}
}  // namespace

/**
 * A policy enum for how to serialize literal values.
 */
enum class LiteralSerializationPolicy {
    // The default way to serialize. Just serialize whatever literals were given if they are still
    // available, or whatever you parsed them to. This is expected to be able to parse again, since
    // it worked the first time.
    kUnchanged,
    // Serialize any literal value as "?number" or similar. For example "?bool" for any boolean. Use
    // 'debugTypeString()' helper.
    kToDebugTypeString,
    // Serialize any literal value to one canonical value of the given type, with the constraint
    // that the chosen representative value should be parseable in this context. There are some
    // default implementations that will usually work (e.g. using the number 1 almost always works
    // for numbers), but serializers should be careful to think about and test this if their parsers
    // reject certain values.
    kToRepresentativeParseableValue,
};

/**
 * A struct with options for how you want to serialize a match or aggregation expression.
 */
struct SerializationOptions {
    using TokenizeIdentifierFunc = std::function<std::string(StringData)>;

    // The default serialization options for a query shape. No need to redact identifiers for the
    // this purpose. We may do that on the $queryStats read path.
    static const SerializationOptions kRepresentativeQueryShapeSerializeOptions;
    static const SerializationOptions kDebugQueryShapeSerializeOptions;
    static const SerializationOptions kMarkIdentifiers_FOR_TEST;
    static const SerializationOptions kDebugShapeAndMarkIdentifiers_FOR_TEST;

    /**
     * Checks if this SerializationOptions represents the same options as another
     * SerializationOptions. Note it cannot compare whether the two 'transformIdentifiersCallback's
     * are the same - the language purposefully leaves the comparison operator undefined.
     */
    bool operator==(const SerializationOptions& other) const {
        return this->transformIdentifiers == other.transformIdentifiers &&
            this->includePath == other.includePath &&
            // You cannot well determine std::function equivalence in C++, so this is the best we'll
            // do.
            (this->transformIdentifiersCallback == nullptr) ==
            (other.transformIdentifiersCallback == nullptr) &&
            this->literalPolicy == other.literalPolicy && this->verbosity == other.verbosity;
    }

    // Helper function for removing identifiable information (like collection/db names).
    // Note: serializeFieldPath/serializeFieldPathFromString should be used for field
    // names.
    std::string serializeIdentifier(StringData str) const {
        if (transformIdentifiers) {
            return transformIdentifiersCallback(str);
        }
        return str.toString();
    }

    std::string serializeFieldPath(FieldPath path) const {
        if (transformIdentifiers) {
            std::stringstream hmaced;
            for (size_t i = 0; i < path.getPathLength(); ++i) {
                if (i > 0) {
                    hmaced << ".";
                }
                hmaced << transformIdentifiersCallback(path.getFieldName(i));
            }
            return hmaced.str();
        }
        return path.fullPath();
    }

    std::string serializeFieldPathWithPrefix(FieldPath path) const {
        return "$" + serializeFieldPath(path);
    }

    std::string serializeFieldPathFromString(StringData path) const;

    std::vector<std::string> serializeFieldPathFromString(
        const std::vector<std::string>& paths) const {
        std::vector<std::string> result;
        result.reserve(paths.size());
        for (auto& p : paths) {
            result.push_back(serializeFieldPathFromString(p));
        }
        return result;
    }

    // Helper functions for applying hmac to BSONObj. Does not take into account anything to do with
    // MQL semantics, removes all field names and literals in the passed in obj.
    void addHmacedArrayToBuilder(BSONArrayBuilder* bab, std::vector<BSONElement> array) const {
        for (const auto& elem : array) {
            if (elem.type() == BSONType::Object) {
                BSONObjBuilder subObj(bab->subobjStart());
                addHmacedObjToBuilder(&subObj, elem.Obj());
                subObj.done();
            } else if (elem.type() == BSONType::Array) {
                BSONArrayBuilder subArr(bab->subarrayStart());
                addHmacedArrayToBuilder(&subArr, elem.Array());
                subArr.done();
            } else {
                *bab << serializeLiteral(elem);
            }
        }
    }

    void addHmacedObjToBuilder(BSONObjBuilder* bob, BSONObj objToHmac) const {
        for (const auto& elem : objToHmac) {
            auto fieldName = serializeFieldPath(elem.fieldName());
            if (elem.type() == BSONType::Object) {
                BSONObjBuilder subObj(bob->subobjStart(fieldName));
                addHmacedObjToBuilder(&subObj, elem.Obj());
                subObj.done();
            } else if (elem.type() == BSONType::Array) {
                BSONArrayBuilder subArr(bob->subarrayStart(fieldName));
                addHmacedArrayToBuilder(&subArr, elem.Array());
                subArr.done();
            } else {
                appendLiteral(bob, fieldName, elem);
            }
        }
    }

    /**
     * Helper method to call 'serializeLiteral()' on 'e' and append the resulting value to 'bob'
     * using the same name as 'e'.
     */
    void appendLiteral(BSONObjBuilder* bob, const BSONElement& e) const;
    void appendLiteral(BSONObjBuilder* bob, StringData name, const BSONElement& e) const;
    /**
     * Helper method to call 'serializeLiteral()' on 'v' and append the result to 'bob' using field
     * name 'fieldName'.
     */
    void appendLiteral(BSONObjBuilder* bob, StringData fieldName, const ImplicitValue& v) const;

    /**
     * Depending on the configured 'literalPolicy', serializeLiteral will return the appropriate
     * value for adding literals to serialization output:
     * - If 'literalPolicy' is 'kUnchanged', returns the input value unmodified.
     * - If it is 'kToDebugTypeString', computes and returns the type string as a string Value.
     * - If it is 'kToRepresentativeValue', it returns an arbitrary value of the same type as the
     *   one given. For any number, this will be the number 1. For any boolean this will be true.
     *
     * Example usage: BSON("myArg" << options.serializeLiteral(_myArg));
     *
     * TODO SERVER-76330 If you need a different value to make sure it will parse, you should not
     * use this API - but use serializeConstrainedLiteral() instead.
     */
    Value serializeLiteral(const BSONElement& e) const;
    Value serializeLiteral(const ImplicitValue& v) const;

    // 'literalPolicy' is an independent option to serialize in a general format with the aim of
    // similar "shaped" queries serializing to the same object. For example, if set to
    // 'kToDebugTypeString', then the serialization of {a: {$gt: 2}} should result in {a: {$gt:
    // '?number'}}, as will the serialization of {a: {$gt: 3}}.
    //
    // "Literal" here is meant to stand in contrast to expression arguments, as in the $gt
    // expressions in {$and: [{a: {$gt: 3}}, {b: {$gt: 4}}]}. There the only literals are 3 and 4,
    // so the serialization expected for 'kToDebugTypeString' would be {$and: [{a: {$gt:
    // '?number'}}, {b: {$lt: '?number'}}]}.
    LiteralSerializationPolicy literalPolicy = LiteralSerializationPolicy::kUnchanged;

    // If true the caller must set transformIdentifiersCallback. 'transformIdentifiers' if set along
    // with a strategy the redaction strategy will be called on any personal identifiable
    // information (e.g., field paths/names, collection names) encountered before serializing them.
    bool transformIdentifiers = false;
    std::function<std::string(StringData)> transformIdentifiersCallback = defaultHmacStrategy;

    // If set to false, serializes without including the path. For example {a: {$gt: 2}} would
    // serialize as just {$gt: 2}.
    //
    // It is expected that most callers want to set 'includePath' to true to get a correct
    // serialization. Internally, we may set this to false if we have a situation where an outer
    // expression serializes a path and we don't want to repeat the path in the inner expression.
    //
    // For example in {a: {$elemMatch: {$eq: 2}}} the "a" is serialized by the $elemMatch, and
    // should not be serialized by the EQ child.
    // The $elemMatch will serialize {a: {$elemMatch: <recurse>}} and the EQ will serialize just
    // {$eq: 2} instead of its usual {a: {$eq: 2}}.
    bool includePath = true;

    // For aggregation indicate whether we should use the more verbose serialization format.
    boost::optional<ExplainOptions::Verbosity> verbosity = boost::none;

    // If set to true, serializes InMatchExpresions by using the sorted and de-duped list of
    // elements. Otherwise, serializes InMatchExpressions using the original (unsorted) list of
    // elements. This flag has no effect on other types of MatchExpressions.
    bool inMatchExprSortAndDedupElements = true;
};

}  // namespace mongo
