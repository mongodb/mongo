/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/expression_parser.h"

#include <pcrecpp.h>

#include "mongo/base/init.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_expr_eq.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/matcher/schema/json_schema_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/stats/counters.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

namespace {

using namespace mongo;

/**
 * Returns true if subtree contains MatchExpression 'type'.
 */
bool hasNode(const MatchExpression* root, MatchExpression::MatchType type) {
    if (type == root->matchType()) {
        return true;
    }
    for (size_t i = 0; i < root->numChildren(); ++i) {
        if (hasNode(root->getChild(i), type)) {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace mongo {

constexpr StringData AlwaysFalseMatchExpression::kName;
constexpr StringData AlwaysTrueMatchExpression::kName;
constexpr StringData OrMatchExpression::kName;
constexpr StringData AndMatchExpression::kName;
constexpr StringData NorMatchExpression::kName;

/**
 * 'DocumentParseLevel' refers to the current position of the parser as it descends a
 *  MatchExpression tree.
 */
enum class DocumentParseLevel {
    // Indicates that the parser is looking at the root level of the BSON object containing the
    // user's query predicate.
    kPredicateTopLevel,
    // Indicates that match expression nodes in this position will match against the complete
    // user document, as opposed to matching against a nested document or a subdocument inside
    // an array.
    kUserDocumentTopLevel,
    // Indicates that match expression nodes in this position will match against a nested
    // document or a subdocument inside an array.
    kUserSubDocument,
};

namespace {

// Forward declarations.

Status parseSub(StringData name,
                const BSONObj& sub,
                AndMatchExpression* root,
                const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const ExtensionsCallback* extensionsCallback,
                MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                DocumentParseLevel currentLevel);

stdx::function<StatusWithMatchExpression(StringData,
                                         BSONElement,
                                         const boost::intrusive_ptr<ExpressionContext>&,
                                         const ExtensionsCallback*,
                                         MatchExpressionParser::AllowedFeatureSet,
                                         DocumentParseLevel)>
retrievePathlessParser(StringData name);

StatusWithMatchExpression parseRegexElement(StringData name,
                                            BSONElement e,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (e.type() != BSONType::RegEx)
        return {Status(ErrorCodes::BadValue, "not a regex")};

    expCtx->incrementMatchExprCounter("$regex");
    return {stdx::make_unique<RegexMatchExpression>(name, e.regex(), e.regexFlags())};
}

StatusWithMatchExpression parseComparison(
    StringData name,
    std::unique_ptr<ComparisonMatchExpression> cmp,
    BSONElement e,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures) {
    // Non-equality comparison match expressions cannot have a regular expression as the argument.
    // (e.g. {a: {$gt: /b/}} is illegal).
    if (MatchExpression::EQ != cmp->matchType() && BSONType::RegEx == e.type()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Can't have RegEx as arg to predicate over field '" << name
                              << "'."};
    }

    cmp->setCollator(expCtx->getCollator());
    return {std::move(cmp)};
}

/**
 * DBRef fields are ordered in the collection. In the query, we consider an embedded object a query
 * on a DBRef as long as it contains $ref and $id.
 * Required fields: $ref and $id (if incomplete DBRefs are not allowed).
 *
 * If incomplete DBRefs are allowed, we accept the BSON object as long as it contains $ref, $id or
 * $db.
 *
 * Field names are checked but not field types.
 *
 * { $ref: "s", $id: "x" } = true
 * { $ref : "s" } = true (if incomplete DBRef is allowed)
 * { $id : "x" } = true (if incomplete DBRef is allowed)
 * { $db : "x" } = true (if incomplete DBRef is allowed)
 */
bool isDBRefDocument(const BSONObj& obj, bool allowIncompleteDBRef) {
    bool hasRef = false;
    bool hasID = false;
    bool hasDB = false;

    BSONObjIterator i(obj);
    while (i.more() && !(hasRef && hasID)) {
        auto element = i.next();
        auto fieldName = element.fieldNameStringData();
        // $ref
        if (!hasRef && "$ref"_sd == fieldName) {
            hasRef = true;
        }
        // $id
        else if (!hasID && "$id"_sd == fieldName) {
            hasID = true;
        }
        // $db
        else if (!hasDB && "$db"_sd == fieldName) {
            hasDB = true;
        }
    }

    if (allowIncompleteDBRef) {
        return hasRef || hasID || hasDB;
    }

    return hasRef && hasID;
}

/**
 * 5 = false
 * { a : 5 } = false
 * { $lt : 5 } = true
 * { $ref: "s", $id: "x" } = false
 * { $ref: "s", $id: "x", $db: "mydb" } = false
 * { $ref : "s" } = false (if incomplete DBRef is allowed)
 * { $id : "x" } = false (if incomplete DBRef is allowed)
 * { $db : "mydb" } = false (if incomplete DBRef is allowed)
 */
bool isExpressionDocument(BSONElement e, bool allowIncompleteDBRef) {
    if (e.type() != BSONType::Object)
        return false;

    auto o = e.Obj();
    if (o.isEmpty())
        return false;

    auto name = o.firstElement().fieldNameStringData();
    if (name[0] != '$')
        return false;

    if (isDBRefDocument(o, allowIncompleteDBRef)) {
        return false;
    }

    return true;
}

/**
 * Parse 'obj' and return either a MatchExpression or an error.
 */
StatusWithMatchExpression parse(const BSONObj& obj,
                                const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                const ExtensionsCallback* extensionsCallback,
                                MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                                DocumentParseLevel currentLevel) {
    auto root = stdx::make_unique<AndMatchExpression>();

    const DocumentParseLevel nextLevel = (currentLevel == DocumentParseLevel::kPredicateTopLevel)
        ? DocumentParseLevel::kUserDocumentTopLevel
        : currentLevel;

    for (auto e : obj) {
        if (e.fieldName()[0] == '$') {
            auto name = e.fieldNameStringData().substr(1);
            auto parseExpressionMatchFunction = retrievePathlessParser(name);

            if (!parseExpressionMatchFunction) {
                return {Status(ErrorCodes::BadValue,
                               str::stream()
                                   << "unknown top level operator: " << e.fieldNameStringData())};
            }

            auto parsedExpression = parseExpressionMatchFunction(
                name, e, expCtx, extensionsCallback, allowedFeatures, currentLevel);

            if (!parsedExpression.isOK()) {
                return parsedExpression;
            }

            // A nullptr for 'parsedExpression' indicates that the particular operator should not
            // be added to 'root', because it is handled outside of the MatchExpressionParser
            // library. The following operators currently follow this convention:
            //    - $comment  has no action associated with the operator.
            if (parsedExpression.getValue().get()) {
                root->add(parsedExpression.getValue().release());
            }

            expCtx->incrementMatchExprCounter(e.fieldNameStringData());
            continue;
        }

        if (isExpressionDocument(e, false)) {
            auto s = parseSub(e.fieldNameStringData(),
                              e.Obj(),
                              root.get(),
                              expCtx,
                              extensionsCallback,
                              allowedFeatures,
                              nextLevel);
            if (!s.isOK())
                return s;
            continue;
        }

        if (e.type() == BSONType::RegEx) {
            auto result = parseRegexElement(e.fieldNameStringData(), e, expCtx);
            if (!result.isOK())
                return result;
            root->add(result.getValue().release());
            continue;
        }

        auto eq =
            parseComparison(e.fieldNameStringData(),
                            stdx::make_unique<EqualityMatchExpression>(e.fieldNameStringData(), e),
                            e,
                            expCtx,
                            allowedFeatures);
        if (!eq.isOK())
            return eq;

        expCtx->incrementMatchExprCounter("$eq");
        root->add(eq.getValue().release());
    }

    if (root->numChildren() == 1) {
        std::unique_ptr<MatchExpression> real(root->getChild(0));
        root->clearAndRelease();
        return {std::move(real)};
    }

    return {std::move(root)};
}

StatusWithMatchExpression parseComment(StringData name,
                                       BSONElement elem,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       const ExtensionsCallback* extensionsCallback,
                                       MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                                       DocumentParseLevel currentLevel) {
    return {nullptr};
}

StatusWithMatchExpression parseWhere(StringData name,
                                     BSONElement elem,
                                     const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     const ExtensionsCallback* extensionsCallback,
                                     MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                                     DocumentParseLevel currentLevel) {
    if ((allowedFeatures & MatchExpressionParser::AllowedFeatures::kJavascript) == 0u) {
        return {Status(ErrorCodes::BadValue, "$where is not allowed in this context")};
    }

    return extensionsCallback->parseWhere(elem);
}

StatusWithMatchExpression parseText(StringData name,
                                    BSONElement elem,
                                    const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const ExtensionsCallback* extensionsCallback,
                                    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                                    DocumentParseLevel currentLevel) {
    if (currentLevel == DocumentParseLevel::kUserSubDocument) {
        return {
            Status(ErrorCodes::BadValue, "$text can only be applied to the top-level document")};
    }

    if ((allowedFeatures & MatchExpressionParser::AllowedFeatures::kText) == 0u) {
        return {Status(ErrorCodes::BadValue, "$text is not allowed in this context")};
    }

    return extensionsCallback->parseText(elem);
}

StatusWithMatchExpression parseDBRef(StringData name,
                                     BSONElement elem,
                                     const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     const ExtensionsCallback* extensionsCallback,
                                     MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                                     DocumentParseLevel currentLevel) {
    auto eq = stdx::make_unique<EqualityMatchExpression>(elem.fieldName(), elem);

    // 'id' is collation-aware. 'ref' and 'db' are compared using binary comparison.
    eq->setCollator("id"_sd == name ? expCtx->getCollator() : nullptr);

    return {std::move(eq)};
}

StatusWithMatchExpression parseJSONSchema(StringData name,
                                          BSONElement elem,
                                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          const ExtensionsCallback* extensionsCallback,
                                          MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                                          DocumentParseLevel currentLevel) {
    if ((allowedFeatures & MatchExpressionParser::AllowedFeatures::kJSONSchema) == 0u) {
        return Status(ErrorCodes::QueryFeatureNotAllowed,
                      "$jsonSchema is not allowed in this context");
    }

    if (elem.type() != BSONType::Object) {
        return {Status(ErrorCodes::TypeMismatch, "$jsonSchema must be an object")};
    }

    return JSONSchemaParser::parse(
        expCtx, elem.Obj(), allowedFeatures, internalQueryIgnoreUnknownJSONSchemaKeywords.load());
}

template <class T>
StatusWithMatchExpression parseAlwaysBoolean(
    StringData name,
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback* extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    DocumentParseLevel currentLevel) {
    auto statusWithLong = elem.parseIntegerElementToLong();
    if (!statusWithLong.isOK()) {
        return statusWithLong.getStatus();
    }

    if (statusWithLong.getValue() != 1) {
        return {Status(ErrorCodes::FailedToParse,
                       str::stream() << T::kName << " must be an integer value of 1")};
    }

    return {stdx::make_unique<T>()};
}

StatusWithMatchExpression parseExpr(StringData name,
                                    BSONElement elem,
                                    const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const ExtensionsCallback* extensionsCallback,
                                    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                                    DocumentParseLevel currentLevel) {
    if (currentLevel == DocumentParseLevel::kUserSubDocument) {
        return {
            Status(ErrorCodes::BadValue, "$expr can only be applied to the top-level document")};
    }

    if ((allowedFeatures & MatchExpressionParser::AllowedFeatures::kExpr) == 0u) {
        return {Status(ErrorCodes::QueryFeatureNotAllowed, "$expr is not allowed in this context")};
    }

    return {stdx::make_unique<ExprMatchExpression>(std::move(elem), expCtx)};
}

StatusWithMatchExpression parseMOD(StringData name, BSONElement e) {
    if (e.type() != BSONType::Array)
        return {Status(ErrorCodes::BadValue, "malformed mod, needs to be an array")};

    BSONObjIterator i(e.Obj());

    if (!i.more())
        return {Status(ErrorCodes::BadValue, "malformed mod, not enough elements")};
    auto d = i.next();
    if (!d.isNumber())
        return {Status(ErrorCodes::BadValue, "malformed mod, divisor not a number")};

    if (!i.more())
        return {Status(ErrorCodes::BadValue, "malformed mod, not enough elements")};
    auto r = i.next();
    if (!r.isNumber())
        return {Status(ErrorCodes::BadValue, "malformed mod, remainder not a number")};

    if (i.more())
        return {Status(ErrorCodes::BadValue, "malformed mod, too many elements")};

    return {std::make_unique<ModMatchExpression>(
        name, ModMatchExpression::truncateToLong(d), ModMatchExpression::truncateToLong(r))};
}

StatusWithMatchExpression parseRegexDocument(StringData name, const BSONObj& doc) {
    StringData regex;
    StringData regexOptions;

    for (auto e : doc) {
        auto matchType = MatchExpressionParser::parsePathAcceptingKeyword(e);
        if (!matchType) {
            continue;
        }

        switch (*matchType) {
            case PathAcceptingKeyword::REGEX:
                if (e.type() == BSONType::String) {
                    regex = e.valueStringData();
                } else if (e.type() == BSONType::RegEx) {
                    regex = e.regex();
                    if (!StringData{e.regexFlags()}.empty()) {
                        if (!regexOptions.empty()) {
                            return {Status(ErrorCodes::Error(51074),
                                           "options set in both $regex and $options")};
                        }
                        regexOptions = e.regexFlags();
                    }
                } else {
                    return {Status(ErrorCodes::BadValue, "$regex has to be a string")};
                }

                break;
            case PathAcceptingKeyword::OPTIONS:
                if (e.type() != BSONType::String) {
                    return {Status(ErrorCodes::BadValue, "$options has to be a string")};
                }

                if (!regexOptions.empty()) {
                    return {Status(ErrorCodes::Error(51075),
                                   "options set in both $regex and $options")};
                }

                regexOptions = e.valueStringData();
                break;
            default:
                break;
        }
    }

    return {stdx::make_unique<RegexMatchExpression>(name, regex, regexOptions)};
}

Status parseInExpression(InMatchExpression* inExpression,
                         const BSONObj& theArray,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    inExpression->setCollator(expCtx->getCollator());
    std::vector<BSONElement> equalities;
    for (auto e : theArray) {
        // Allow DBRefs, but reject all fields with names starting with $.
        if (isExpressionDocument(e, false)) {
            return Status(ErrorCodes::BadValue, "cannot nest $ under $in");
        }

        if (e.type() == BSONType::RegEx) {
            auto status = inExpression->addRegex(stdx::make_unique<RegexMatchExpression>(""_sd, e));
            if (!status.isOK()) {
                return status;
            }
        } else {
            equalities.push_back(e);
        }
    }
    return inExpression->setEqualities(std::move(equalities));
}

template <class T>
StatusWithMatchExpression parseType(StringData name, BSONElement elt) {
    auto typeSet = MatcherTypeSet::parse(elt, kTypeAliasMap);
    if (!typeSet.isOK()) {
        return typeSet.getStatus();
    }

    if (typeSet.getValue().isEmpty()) {
        return {Status(ErrorCodes::FailedToParse,
                       str::stream() << name << " must match at least one type")};
    }

    return {stdx::make_unique<T>(name, std::move(typeSet.getValue()))};
}

/**
 * Converts 'theArray', a BSONArray of integers, into a std::vector of integers.
 */
StatusWith<std::vector<uint32_t>> parseBitPositionsArray(const BSONObj& theArray) {
    std::vector<uint32_t> bitPositions;

    // Fill temporary bit position array with integers read from the BSON array.
    for (auto e : theArray) {
        if (!e.isNumber()) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "bit positions must be an integer but got: " << e);
        }

        if (e.type() == BSONType::NumberDouble) {
            auto eDouble = e.numberDouble();

            // NaN doubles are rejected.
            if (std::isnan(eDouble)) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "bit positions cannot take a NaN: " << e);
            }

            // This makes sure e does not overflow a 32-bit integer container.
            if (eDouble > std::numeric_limits<int>::max() ||
                eDouble < std::numeric_limits<int>::min()) {
                return Status(
                    ErrorCodes::BadValue,
                    str::stream()
                        << "bit positions cannot be represented as a 32-bit signed integer: " << e);
            }

            // This checks if e is integral.
            if (eDouble != static_cast<double>(static_cast<long long>(eDouble))) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "bit positions must be an integer but got: " << e);
            }
        }

        if (e.type() == BSONType::NumberLong) {
            auto eLong = e.numberLong();

            // This makes sure e does not overflow a 32-bit integer container.
            if (eLong > std::numeric_limits<int>::max() ||
                eLong < std::numeric_limits<int>::min()) {
                return Status(
                    ErrorCodes::BadValue,
                    str::stream()
                        << "bit positions cannot be represented as a 32-bit signed integer: " << e);
            }
        }

        auto eValue = e.numberInt();

        // No negatives.
        if (eValue < 0) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "bit positions must be >= 0 but got: " << e);
        }

        bitPositions.push_back(eValue);
    }

    return bitPositions;
}

/**
 * Parses 'e' into a BitTestMatchExpression.
 */
template <class T>
StatusWithMatchExpression parseBitTest(StringData name, BSONElement e) {
    std::unique_ptr<BitTestMatchExpression> bitTestMatchExpression;

    if (e.type() == BSONType::Array) {
        // Array of bit positions provided as value.
        auto bitPositions = parseBitPositionsArray(e.Obj());
        if (!bitPositions.isOK()) {
            return bitPositions.getStatus();
        }
        bitTestMatchExpression = stdx::make_unique<T>(name, std::move(bitPositions.getValue()));
    } else if (e.isNumber()) {
        // Integer bitmask provided as value.
        auto bitMask = e.parseIntegerElementToNonNegativeLong();
        if (!bitMask.isOK()) {
            return bitMask.getStatus();
        }
        bitTestMatchExpression = stdx::make_unique<T>(name, bitMask.getValue());
    } else if (e.type() == BSONType::BinData) {
        // Binary bitmask provided as value.
        int eBinaryLen;
        auto eBinary = e.binData(eBinaryLen);
        bitTestMatchExpression = stdx::make_unique<T>(name, eBinary, eBinaryLen);
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << name << " takes an Array, a number, or a BinData but received: " << e);
    }

    return {std::move(bitTestMatchExpression)};
}

StatusWithMatchExpression parseInternalSchemaFmod(StringData name, BSONElement elem) {
    StringData path(name);
    if (elem.type() != BSONType::Array)
        return {ErrorCodes::BadValue,
                str::stream() << path << " must be an array, but got type " << elem.type()};

    BSONObjIterator i(elem.embeddedObject());
    if (!i.more()) {
        return {ErrorCodes::BadValue, str::stream() << path << " does not have enough elements"};
    }
    auto d = i.next();
    if (!d.isNumber()) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << path << " does not have a numeric divisor"};
    }

    if (!i.more()) {
        return {ErrorCodes::BadValue, str::stream() << path << " does not have enough elements"};
    }
    auto r = i.next();
    if (!d.isNumber()) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << path << " does not have a numeric remainder"};
    }

    if (i.more()) {
        return {ErrorCodes::BadValue, str::stream() << path << " has too many elements"};
    }

    return {stdx::make_unique<InternalSchemaFmodMatchExpression>(
        name, d.numberDecimal(), r.numberDecimal())};
}

StatusWithMatchExpression parseInternalSchemaRootDocEq(
    StringData name,
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback* extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    DocumentParseLevel currentLevel) {
    if (currentLevel == DocumentParseLevel::kUserSubDocument) {
        return {Status(ErrorCodes::FailedToParse,
                       str::stream() << InternalSchemaRootDocEqMatchExpression::kName
                                     << " can only be applied to the top level document")};
    }

    if (elem.type() != BSONType::Object) {
        return {Status(ErrorCodes::TypeMismatch,
                       str::stream() << InternalSchemaRootDocEqMatchExpression::kName
                                     << " must be an object, found type " << elem.type())};
    }
    auto rootDocEq =
        stdx::make_unique<InternalSchemaRootDocEqMatchExpression>(elem.embeddedObject());
    return {std::move(rootDocEq)};
}

/**
 * Parses the given BSONElement into a single integer argument and creates a MatchExpression
 * of type 'T' that gets initialized with the resulting integer.
 */
template <class T>
StatusWithMatchExpression parseInternalSchemaSingleIntegerArgument(StringData name,
                                                                   BSONElement elem) {
    auto parsedInt = elem.parseIntegerElementToNonNegativeLong();
    if (!parsedInt.isOK()) {
        return parsedInt.getStatus();
    }

    return {stdx::make_unique<T>(name, parsedInt.getValue())};
}

/**
 * Same as the parseInternalSchemaSingleIntegerArgument function, but for top-level
 * operators which don't have paths.
 */
template <class T>
StatusWithMatchExpression parseTopLevelInternalSchemaSingleIntegerArgument(
    StringData name,
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback* extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    DocumentParseLevel currentLevel) {
    auto parsedInt = elem.parseIntegerElementToNonNegativeLong();
    if (!parsedInt.isOK()) {
        return parsedInt.getStatus();
    }
    return {stdx::make_unique<T>(parsedInt.getValue())};
}

/**
 * Looks at the field named 'namePlaceholderFieldName' within 'containingObject' and parses a name
 * placeholder from that element. 'expressionName' is the name of the expression that requires the
 * name placeholder and is used to generate helpful error messages.
 */
StatusWith<StringData> parseNamePlaceholder(const BSONObj& containingObject,
                                            StringData namePlaceholderFieldName,
                                            StringData expressionName) {
    auto namePlaceholderElem = containingObject[namePlaceholderFieldName];
    if (!namePlaceholderElem) {
        return {ErrorCodes::FailedToParse,
                str::stream() << expressionName << " requires a '" << namePlaceholderFieldName
                              << "'"};
    } else if (namePlaceholderElem.type() != BSONType::String) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << expressionName << " requires '" << namePlaceholderFieldName
                              << "' to be a string, not " << namePlaceholderElem.type()};
    }
    return {namePlaceholderElem.valueStringData()};
}

/**
 * Looks at the field named 'exprWithPlaceholderFieldName' within 'containingObject' and parses an
 * ExpressionWithPlaceholder from that element. Fails if an error occurs during parsing, or if the
 * ExpressionWithPlaceholder has a different name placeholder than 'expectedPlaceholder'.
 * 'expressionName' is the name of the expression that requires the ExpressionWithPlaceholder and is
 * used to generate helpful error messages.
 */
StatusWith<std::unique_ptr<ExpressionWithPlaceholder>> parseExprWithPlaceholder(
    const BSONObj& containingObject,
    StringData exprWithPlaceholderFieldName,
    StringData expressionName,
    StringData expectedPlaceholder,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback* extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    DocumentParseLevel currentLevel) {
    auto exprWithPlaceholderElem = containingObject[exprWithPlaceholderFieldName];
    if (!exprWithPlaceholderElem) {
        return {ErrorCodes::FailedToParse,
                str::stream() << expressionName << " requires '" << exprWithPlaceholderFieldName
                              << "'"};
    } else if (exprWithPlaceholderElem.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << expressionName << " found '" << exprWithPlaceholderFieldName
                              << "', which is an incompatible type: "
                              << exprWithPlaceholderElem.type()};
    }

    auto filter = parse(exprWithPlaceholderElem.embeddedObject(),
                        expCtx,
                        extensionsCallback,
                        MatchExpressionParser::kBanAllSpecialFeatures,
                        currentLevel);

    if (!filter.isOK()) {
        return filter.getStatus();
    }

    auto result = ExpressionWithPlaceholder::make(std::move(filter.getValue()));
    if (!result.isOK()) {
        return result.getStatus();
    }

    auto placeholder = result.getValue()->getPlaceholder();
    if (placeholder && (*placeholder != expectedPlaceholder)) {
        return {ErrorCodes::FailedToParse,
                str::stream() << expressionName << " expected a name placeholder of "
                              << expectedPlaceholder << ", but '"
                              << exprWithPlaceholderElem.fieldNameStringData()
                              << "' has a mismatching placeholder '" << *placeholder << "'"};
    }
    return result;
}

StatusWith<std::vector<InternalSchemaAllowedPropertiesMatchExpression::PatternSchema>>
parsePatternProperties(BSONElement patternPropertiesElem,
                       StringData expectedPlaceholder,
                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       const ExtensionsCallback* extensionsCallback,
                       MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                       DocumentParseLevel currentLevel) {
    if (!patternPropertiesElem) {
        return {ErrorCodes::FailedToParse,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires 'patternProperties'"};
    } else if (patternPropertiesElem.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires 'patternProperties' to be an array, not "
                              << patternPropertiesElem.type()};
    }

    std::vector<InternalSchemaAllowedPropertiesMatchExpression::PatternSchema> patternProperties;
    for (auto constraintElem : patternPropertiesElem.embeddedObject()) {
        if (constraintElem.type() != BSONType::Object) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                                  << " requires 'patternProperties' to be an array of objects"};
        }

        auto constraint = constraintElem.embeddedObject();
        if (constraint.nFields() != 2) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                                  << " requires 'patternProperties' to be an array of objects "
                                     "containing exactly two fields, 'regex' and 'expression'"};
        }

        auto expressionWithPlaceholder =
            parseExprWithPlaceholder(constraint,
                                     "expression"_sd,
                                     InternalSchemaAllowedPropertiesMatchExpression::kName,
                                     expectedPlaceholder,
                                     expCtx,
                                     extensionsCallback,
                                     allowedFeatures,
                                     currentLevel);
        if (!expressionWithPlaceholder.isOK()) {
            return expressionWithPlaceholder.getStatus();
        }

        auto regexElem = constraint["regex"];
        if (!regexElem) {
            return {
                ErrorCodes::FailedToParse,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires each object in 'patternProperties' to have a 'regex'"};
        }
        if (regexElem.type() != BSONType::RegEx) {
            return {ErrorCodes::TypeMismatch,
                    str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                                  << " requires 'patternProperties' to be an array of objects, "
                                     "where 'regex' is a regular expression"};
        } else if (*regexElem.regexFlags() != '\0') {
            return {
                ErrorCodes::BadValue,
                str::stream()
                    << InternalSchemaAllowedPropertiesMatchExpression::kName
                    << " does not accept regex flags for pattern schemas in 'patternProperties'"};
        }

        patternProperties.emplace_back(
            InternalSchemaAllowedPropertiesMatchExpression::Pattern(regexElem.regex()),
            std::move(expressionWithPlaceholder.getValue()));
    }

    return std::move(patternProperties);
}

StatusWith<StringDataSet> parseProperties(BSONElement propertiesElem) {
    if (!propertiesElem) {
        return {ErrorCodes::FailedToParse,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires 'properties' to be present"};
    } else if (propertiesElem.type() != BSONType::Array) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires 'properties' to be an array, not "
                              << propertiesElem.type()};
    }

    StringDataSet properties;
    for (auto property : propertiesElem.embeddedObject()) {
        if (property.type() != BSONType::String) {
            return {
                ErrorCodes::TypeMismatch,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires 'properties' to be an array of strings, but found a "
                              << property.type()};
        }
        properties.insert(property.valueStringData());
    }

    return std::move(properties);
}

StatusWithMatchExpression parseInternalSchemaAllowedProperties(
    StringData name,
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback* extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    DocumentParseLevel currentLevel) {
    if (elem.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " must be an object"};
    }

    auto subobj = elem.embeddedObject();
    if (subobj.nFields() != 4) {
        return {ErrorCodes::FailedToParse,
                str::stream() << InternalSchemaAllowedPropertiesMatchExpression::kName
                              << " requires exactly four fields: 'properties', 'namePlaceholder', "
                                 "'patternProperties' and 'otherwise'"};
    }

    auto namePlaceholder = parseNamePlaceholder(
        subobj, "namePlaceholder"_sd, InternalSchemaAllowedPropertiesMatchExpression::kName);
    if (!namePlaceholder.isOK()) {
        return namePlaceholder.getStatus();
    }

    auto patternProperties = parsePatternProperties(subobj["patternProperties"],
                                                    namePlaceholder.getValue(),
                                                    expCtx,
                                                    extensionsCallback,
                                                    allowedFeatures,
                                                    currentLevel);
    if (!patternProperties.isOK()) {
        return patternProperties.getStatus();
    }

    auto otherwise = parseExprWithPlaceholder(subobj,
                                              "otherwise"_sd,
                                              InternalSchemaAllowedPropertiesMatchExpression::kName,
                                              namePlaceholder.getValue(),
                                              expCtx,
                                              extensionsCallback,
                                              allowedFeatures,
                                              currentLevel);
    if (!otherwise.isOK()) {
        return otherwise.getStatus();
    }

    auto properties = parseProperties(subobj["properties"]);
    if (!properties.isOK()) {
        return properties.getStatus();
    }

    return {stdx::make_unique<InternalSchemaAllowedPropertiesMatchExpression>(
        std::move(properties.getValue()),
        namePlaceholder.getValue(),
        std::move(patternProperties.getValue()),
        std::move(otherwise.getValue()))};
}

/**
 * Parses 'elem' into an InternalSchemaMatchArrayIndexMatchExpression.
 */
StatusWithMatchExpression parseInternalSchemaMatchArrayIndex(
    StringData path,
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback* extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    DocumentParseLevel currentLevel) {
    if (elem.type() != BSONType::Object) {
        return {ErrorCodes::TypeMismatch,
                str::stream() << InternalSchemaMatchArrayIndexMatchExpression::kName
                              << " must be an object"};
    }

    auto subobj = elem.embeddedObject();
    if (subobj.nFields() != 3) {
        return {ErrorCodes::FailedToParse,
                str::stream() << InternalSchemaMatchArrayIndexMatchExpression::kName
                              << " requires exactly three fields: 'index', "
                                 "'namePlaceholder' and 'expression'"};
    }

    auto index = subobj["index"].parseIntegerElementToNonNegativeLong();
    if (!index.isOK()) {
        return index.getStatus();
    }

    auto namePlaceholder = parseNamePlaceholder(
        subobj, "namePlaceholder"_sd, InternalSchemaMatchArrayIndexMatchExpression::kName);
    if (!namePlaceholder.isOK()) {
        return namePlaceholder.getStatus();
    }

    auto expressionWithPlaceholder =
        parseExprWithPlaceholder(subobj,
                                 "expression"_sd,
                                 InternalSchemaMatchArrayIndexMatchExpression::kName,
                                 namePlaceholder.getValue(),
                                 expCtx,
                                 extensionsCallback,
                                 allowedFeatures,
                                 currentLevel);
    if (!expressionWithPlaceholder.isOK()) {
        return expressionWithPlaceholder.getStatus();
    }

    return {stdx::make_unique<InternalSchemaMatchArrayIndexMatchExpression>(
        path, index.getValue(), std::move(expressionWithPlaceholder.getValue()))};
}

StatusWithMatchExpression parseGeo(StringData name,
                                   PathAcceptingKeyword type,
                                   const BSONObj& section,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   MatchExpressionParser::AllowedFeatureSet allowedFeatures) {
    if (PathAcceptingKeyword::WITHIN == type || PathAcceptingKeyword::GEO_INTERSECTS == type) {
        auto gq = stdx::make_unique<GeoExpression>(name.toString());
        auto parseStatus = gq->parseFrom(section);
        if (!parseStatus.isOK()) {
            return parseStatus;
        }
        return {stdx::make_unique<GeoMatchExpression>(name, gq.release(), section)};
    } else {
        invariant(PathAcceptingKeyword::GEO_NEAR == type);

        if ((allowedFeatures & MatchExpressionParser::AllowedFeatures::kGeoNear) == 0u) {
            return {Status(ErrorCodes::BadValue,
                           "$geoNear, $near, and $nearSphere are not allowed in this context")};
        }

        auto nq = stdx::make_unique<GeoNearExpression>(name.toString());
        auto status = nq->parseFrom(section);
        if (!status.isOK()) {
            return status;
        }
        expCtx->incrementMatchExprCounter(section.firstElementFieldName());
        return {stdx::make_unique<GeoNearMatchExpression>(name, nq.release(), section)};
    }
}

template <class T>
StatusWithMatchExpression parseTreeTopLevel(
    StringData name,
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback* extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    DocumentParseLevel currentLevel) {
    if (elem.type() != BSONType::Array) {
        return {Status(ErrorCodes::BadValue, str::stream() << T::kName << " must be an array")};
    }

    auto temp = stdx::make_unique<T>();

    auto arr = elem.Obj();
    if (arr.isEmpty()) {
        return Status(ErrorCodes::BadValue, "$and/$or/$nor must be a nonempty array");
    }

    for (auto e : arr) {
        if (e.type() != BSONType::Object)
            return Status(ErrorCodes::BadValue, "$or/$and/$nor entries need to be full objects");

        auto sub = parse(e.Obj(), expCtx, extensionsCallback, allowedFeatures, currentLevel);
        if (!sub.isOK())
            return sub.getStatus();

        temp->add(sub.getValue().release());
    }

    return {std::move(temp)};
}

StatusWithMatchExpression parseElemMatch(StringData name,
                                         BSONElement e,
                                         const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                         const ExtensionsCallback* extensionsCallback,
                                         MatchExpressionParser::AllowedFeatureSet allowedFeatures) {
    if (e.type() != BSONType::Object)
        return {Status(ErrorCodes::BadValue, "$elemMatch needs an Object")};

    auto obj = e.Obj();

    // $elemMatch value case applies when the children all
    // work on the field 'name'.
    // This is the case when:
    //     1) the argument is an expression document; and
    //     2) expression is not a AND/NOR/OR logical operator. Children of
    //        these logical operators are initialized with field names.
    //     3) expression is not a WHERE operator. WHERE works on objects instead
    //        of specific field.
    bool isElemMatchValue = false;
    if (isExpressionDocument(e, true)) {
        auto elt = obj.firstElement();
        invariant(elt);

        isElemMatchValue = !retrievePathlessParser(elt.fieldNameStringData().substr(1));
    }

    if (isElemMatchValue) {
        // Value case.

        AndMatchExpression theAnd;
        auto s = parseSub("",
                          obj,
                          &theAnd,
                          expCtx,
                          extensionsCallback,
                          allowedFeatures,
                          DocumentParseLevel::kUserSubDocument);
        if (!s.isOK())
            return s;

        auto temp = stdx::make_unique<ElemMatchValueMatchExpression>(name);

        for (size_t i = 0; i < theAnd.numChildren(); i++) {
            temp->add(theAnd.getChild(i));
        }
        theAnd.clearAndRelease();

        return {std::move(temp)};
    }

    // DBRef value case
    // A DBRef document under a $elemMatch should be treated as an object case because it may
    // contain non-DBRef fields in addition to $ref, $id and $db.

    // Object case.

    auto subRaw = parse(
        obj, expCtx, extensionsCallback, allowedFeatures, DocumentParseLevel::kUserSubDocument);
    if (!subRaw.isOK())
        return subRaw;
    auto sub = std::move(subRaw.getValue());

    // $where is not supported under $elemMatch because $where applies to top-level document, not
    // array elements in a field.
    if (hasNode(sub.get(), MatchExpression::WHERE)) {
        return {Status(ErrorCodes::BadValue, "$elemMatch cannot contain $where expression")};
    }

    return {stdx::make_unique<ElemMatchObjectMatchExpression>(name, sub.release())};
}

StatusWithMatchExpression parseAll(StringData name,
                                   BSONElement e,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   const ExtensionsCallback* extensionsCallback,
                                   MatchExpressionParser::AllowedFeatureSet allowedFeatures) {
    if (e.type() != BSONType::Array)
        return {Status(ErrorCodes::BadValue, "$all needs an array")};

    auto arr = e.Obj();
    auto myAnd = stdx::make_unique<AndMatchExpression>();
    BSONObjIterator i(arr);

    if (arr.firstElement().type() == BSONType::Object &&
        "$elemMatch"_sd == arr.firstElement().Obj().firstElement().fieldNameStringData()) {
        // $all : [ { $elemMatch : {} } ... ]

        while (i.more()) {
            auto hopefullyElemMatchElement = i.next();

            if (hopefullyElemMatchElement.type() != BSONType::Object) {
                // $all : [ { $elemMatch : ... }, 5 ]
                return {Status(ErrorCodes::BadValue, "$all/$elemMatch has to be consistent")};
            }

            auto hopefullyElemMatchObj = hopefullyElemMatchElement.Obj();
            if ("$elemMatch"_sd != hopefullyElemMatchObj.firstElement().fieldNameStringData()) {
                // $all : [ { $elemMatch : ... }, { x : 5 } ]
                return {Status(ErrorCodes::BadValue, "$all/$elemMatch has to be consistent")};
            }

            auto inner = parseElemMatch(name,
                                        hopefullyElemMatchObj.firstElement(),
                                        expCtx,
                                        extensionsCallback,
                                        allowedFeatures);
            if (!inner.isOK())
                return inner;
            myAnd->add(inner.getValue().release());
        }

        return {std::move(myAnd)};
    }

    while (i.more()) {
        auto e = i.next();

        if (e.type() == BSONType::RegEx) {
            auto expr = stdx::make_unique<RegexMatchExpression>(name, e);
            myAnd->add(expr.release());
        } else if (e.type() == BSONType::Object &&
                   MatchExpressionParser::parsePathAcceptingKeyword(e.Obj().firstElement())) {
            return {Status(ErrorCodes::BadValue, "no $ expressions in $all")};
        } else {
            auto expr = stdx::make_unique<EqualityMatchExpression>(name, e);
            expr->setCollator(expCtx->getCollator());
            myAnd->add(expr.release());
        }
    }

    if (myAnd->numChildren() == 0) {
        return {stdx::make_unique<AlwaysFalseMatchExpression>()};
    }

    return {std::move(myAnd)};
}

/**
 * Parses a MatchExpression which takes a fixed-size array of MatchExpressions as arguments.
 */
template <class T>
StatusWithMatchExpression parseInternalSchemaFixedArityArgument(
    StringData name,
    BSONElement elem,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback* extensionsCallback,
    MatchExpressionParser::AllowedFeatureSet allowedFeatures,
    DocumentParseLevel currentLevel) {
    constexpr auto arity = T::arity();
    if (elem.type() != BSONType::Array) {
        return {ErrorCodes::FailedToParse,
                str::stream() << elem.fieldNameStringData() << " must be an array of " << arity
                              << " MatchExpressions"};
    }

    auto inputObj = elem.embeddedObject();
    if (static_cast<size_t>(inputObj.nFields()) != arity) {
        return {ErrorCodes::FailedToParse,
                str::stream() << elem.fieldNameStringData() << " requires exactly " << arity
                              << " MatchExpressions, but got " << inputObj.nFields()};
    }

    // Fill out 'expressions' with all of the parsed subexpressions contained in the array,
    // tracking our location in the array with 'position'.
    std::array<std::unique_ptr<MatchExpression>, arity> expressions;
    auto position = expressions.begin();

    for (auto obj : inputObj) {
        if (obj.type() != BSONType::Object) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << elem.fieldNameStringData()
                                  << " must be an array of objects, but found an element of type "
                                  << obj.type()};
        }

        auto subexpr =
            parse(obj.embeddedObject(), expCtx, extensionsCallback, allowedFeatures, currentLevel);
        if (!subexpr.isOK()) {
            return subexpr.getStatus();
        }
        *position = std::move(subexpr.getValue());
        ++position;
    }

    return {stdx::make_unique<T>(std::move(expressions))};
}

StatusWithMatchExpression parseNot(StringData name,
                                   BSONElement elem,
                                   const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   const ExtensionsCallback* extensionsCallback,
                                   MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                                   DocumentParseLevel currentLevel) {
    if (elem.type() == BSONType::RegEx) {
        auto regex = parseRegexElement(name, elem, expCtx);
        if (!regex.isOK()) {
            return regex;
        }
        return {stdx::make_unique<NotMatchExpression>(regex.getValue().release())};
    }

    if (elem.type() != BSONType::Object) {
        return {ErrorCodes::BadValue, "$not needs a regex or a document"};
    }

    auto notObject = elem.Obj();
    if (notObject.isEmpty()) {
        return {ErrorCodes::BadValue, "$not cannot be empty"};
    }

    auto theAnd = stdx::make_unique<AndMatchExpression>();
    auto parseStatus = parseSub(
        name, notObject, theAnd.get(), expCtx, extensionsCallback, allowedFeatures, currentLevel);
    if (!parseStatus.isOK()) {
        return parseStatus;
    }

    return {stdx::make_unique<NotMatchExpression>(theAnd.release())};
}

StatusWithMatchExpression parseInternalSchemaBinDataSubType(StringData name, BSONElement e) {
    if (!e.isNumber()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << InternalSchemaBinDataSubTypeExpression::kName
                                    << " must be represented as a number");
    }

    auto valueAsInt = e.parseIntegerElementToInt();
    if (!valueAsInt.isOK()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "Invalid numerical BinData subtype value for "
                          << InternalSchemaBinDataSubTypeExpression::kName << ": " << e.number());
    }

    if (!isValidBinDataType(valueAsInt.getValue())) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << InternalSchemaBinDataSubTypeExpression::kName
                          << " value must represent BinData subtype: " << valueAsInt.getValue());
    }

    return {stdx::make_unique<InternalSchemaBinDataSubTypeExpression>(
        name, static_cast<BinDataType>(valueAsInt.getValue()))};
}

/**
 * Parses a single field in a sub expression.
 * If the query is { x : { $gt : 5, $lt : 8 } },
 * 'e' is $gt : 5
 */
StatusWithMatchExpression parseSubField(const BSONObj& context,
                                        const AndMatchExpression* andSoFar,
                                        StringData name,
                                        BSONElement e,
                                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                        const ExtensionsCallback* extensionsCallback,
                                        MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                                        DocumentParseLevel currentLevel) {
    invariant(e);

    if ("$eq"_sd == e.fieldNameStringData()) {
        return parseComparison(
            name, stdx::make_unique<EqualityMatchExpression>(name, e), e, expCtx, allowedFeatures);
    }

    if ("$not"_sd == e.fieldNameStringData()) {
        return parseNot(name, e, expCtx, extensionsCallback, allowedFeatures, currentLevel);
    }

    auto parseExpMatchType = MatchExpressionParser::parsePathAcceptingKeyword(e);
    if (!parseExpMatchType) {
        // $where cannot be a sub-expression because it works on top-level documents only.
        if ("$where"_sd == e.fieldNameStringData()) {
            return {Status(ErrorCodes::BadValue, "$where cannot be applied to a field")};
        }

        return {Status(ErrorCodes::BadValue,
                       str::stream() << "unknown operator: " << e.fieldNameStringData())};
    }

    switch (*parseExpMatchType) {
        case PathAcceptingKeyword::LESS_THAN:
            return parseComparison(
                name, stdx::make_unique<LTMatchExpression>(name, e), e, expCtx, allowedFeatures);
        case PathAcceptingKeyword::LESS_THAN_OR_EQUAL:
            return parseComparison(
                name, stdx::make_unique<LTEMatchExpression>(name, e), e, expCtx, allowedFeatures);
        case PathAcceptingKeyword::GREATER_THAN:
            return parseComparison(
                name, stdx::make_unique<GTMatchExpression>(name, e), e, expCtx, allowedFeatures);
        case PathAcceptingKeyword::GREATER_THAN_OR_EQUAL:
            return parseComparison(
                name, stdx::make_unique<GTEMatchExpression>(name, e), e, expCtx, allowedFeatures);
        case PathAcceptingKeyword::NOT_EQUAL: {
            if (BSONType::RegEx == e.type()) {
                // Just because $ne can be rewritten as the negation of an equality does not mean
                // that $ne of a regex is allowed. See SERVER-1705.
                return {Status(ErrorCodes::BadValue, "Can't have regex as arg to $ne.")};
            }
            StatusWithMatchExpression s =
                parseComparison(name,
                                stdx::make_unique<EqualityMatchExpression>(name, e),
                                e,
                                expCtx,
                                allowedFeatures);
            return {stdx::make_unique<NotMatchExpression>(s.getValue().release())};
        }
        case PathAcceptingKeyword::EQUALITY:
            return parseComparison(name,
                                   stdx::make_unique<EqualityMatchExpression>(name, e),
                                   e,
                                   expCtx,
                                   allowedFeatures);

        case PathAcceptingKeyword::IN_EXPR: {
            if (e.type() != BSONType::Array) {
                return {Status(ErrorCodes::BadValue, "$in needs an array")};
            }
            auto temp = stdx::make_unique<InMatchExpression>(name);
            auto parseStatus = parseInExpression(temp.get(), e.Obj(), expCtx);
            if (!parseStatus.isOK()) {
                return parseStatus;
            }
            return {std::move(temp)};
        }

        case PathAcceptingKeyword::NOT_IN: {
            if (e.type() != Array) {
                return {Status(ErrorCodes::BadValue, "$nin needs an array")};
            }
            auto temp = stdx::make_unique<InMatchExpression>(name);
            auto parseStatus = parseInExpression(temp.get(), e.Obj(), expCtx);
            if (!parseStatus.isOK()) {
                return parseStatus;
            }
            return {stdx::make_unique<NotMatchExpression>(temp.release())};
        }

        case PathAcceptingKeyword::SIZE: {
            int size = 0;
            if (e.type() == BSONType::NumberInt) {
                size = e.numberInt();
            } else if (e.type() == BSONType::NumberLong) {
                if (e.numberInt() == e.numberLong()) {
                    size = e.numberInt();
                } else {
                    return {Status(ErrorCodes::BadValue,
                                   "$size must be representable as a 32-bit integer")};
                }
            } else if (e.type() == BSONType::NumberDouble) {
                if (e.numberInt() == e.numberDouble()) {
                    size = e.numberInt();
                } else {
                    return {Status(ErrorCodes::BadValue, "$size must be a whole number")};
                }
            } else {
                return {Status(ErrorCodes::BadValue, "$size needs a number")};
            }

            if (size < 0) {
                return {Status(ErrorCodes::BadValue, "$size may not be negative")};
            }
            return {stdx::make_unique<SizeMatchExpression>(name, size)};
        }

        case PathAcceptingKeyword::EXISTS: {
            if (e.eoo()) {
                return {Status(ErrorCodes::BadValue, "$exists can't be eoo")};
            }

            auto existsExpr = stdx::make_unique<ExistsMatchExpression>(name);
            if (e.trueValue()) {
                return {std::move(existsExpr)};
            }

            return {stdx::make_unique<NotMatchExpression>(existsExpr.release())};
        }

        case PathAcceptingKeyword::TYPE:
            return parseType<TypeMatchExpression>(name, e);

        case PathAcceptingKeyword::MOD:
            return parseMOD(name, e);

        case PathAcceptingKeyword::OPTIONS: {
            // TODO: try to optimize this
            // we have to do this since $options can be before or after a $regex
            // but we validate here
            for (auto temp : context) {
                if (MatchExpressionParser::parsePathAcceptingKeyword(temp) ==
                    PathAcceptingKeyword::REGEX)
                    return {nullptr};
            }

            return {Status(ErrorCodes::BadValue, "$options needs a $regex")};
        }

        case PathAcceptingKeyword::REGEX: {
            return parseRegexDocument(name, context);
        }

        case PathAcceptingKeyword::ELEM_MATCH:
            return parseElemMatch(name, e, expCtx, extensionsCallback, allowedFeatures);

        case PathAcceptingKeyword::ALL:
            return parseAll(name, e, expCtx, extensionsCallback, allowedFeatures);

        case PathAcceptingKeyword::WITHIN:
        case PathAcceptingKeyword::GEO_INTERSECTS:
            return parseGeo(name, *parseExpMatchType, context, expCtx, allowedFeatures);

        case PathAcceptingKeyword::GEO_NEAR:
            return {Status(ErrorCodes::BadValue,
                           str::stream() << "near must be first in: " << context)};

        case PathAcceptingKeyword::INTERNAL_EXPR_EQ: {
            if (e.type() == BSONType::Undefined || e.type() == BSONType::Array) {
                return {Status(ErrorCodes::BadValue,
                               str::stream() << InternalExprEqMatchExpression::kName
                                             << " cannot be used to compare to type: "
                                             << typeName(e.type()))};
            }

            auto exprEqExpr = stdx::make_unique<InternalExprEqMatchExpression>(name, e);
            exprEqExpr->setCollator(expCtx->getCollator());
            return {std::move(exprEqExpr)};
        }

        // Handles bitwise query operators.
        case PathAcceptingKeyword::BITS_ALL_SET: {
            return parseBitTest<BitsAllSetMatchExpression>(name, e);
        }

        case PathAcceptingKeyword::BITS_ALL_CLEAR: {
            return parseBitTest<BitsAllClearMatchExpression>(name, e);
        }

        case PathAcceptingKeyword::BITS_ANY_SET: {
            return parseBitTest<BitsAnySetMatchExpression>(name, e);
        }

        case PathAcceptingKeyword::BITS_ANY_CLEAR: {
            return parseBitTest<BitsAnyClearMatchExpression>(name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_FMOD:
            return parseInternalSchemaFmod(name, e);

        case PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_ITEMS: {
            return parseInternalSchemaSingleIntegerArgument<InternalSchemaMinItemsMatchExpression>(
                name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_ITEMS: {
            return parseInternalSchemaSingleIntegerArgument<InternalSchemaMaxItemsMatchExpression>(
                name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_OBJECT_MATCH: {
            if (e.type() != BSONType::Object) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream() << "$_internalSchemaObjectMatch must be an object");
            }

            auto parsedSubObjExpr = parse(e.Obj(),
                                          expCtx,
                                          extensionsCallback,
                                          allowedFeatures,
                                          DocumentParseLevel::kUserSubDocument);
            if (!parsedSubObjExpr.isOK()) {
                return parsedSubObjExpr;
            }

            return {stdx::make_unique<InternalSchemaObjectMatchExpression>(
                name, std::move(parsedSubObjExpr.getValue()))};
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_UNIQUE_ITEMS: {
            if (!e.isBoolean() || !e.boolean()) {
                return {ErrorCodes::FailedToParse,
                        str::stream() << name << " must be a boolean of value true"};
            }

            return {stdx::make_unique<InternalSchemaUniqueItemsMatchExpression>(name)};
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_LENGTH: {
            return parseInternalSchemaSingleIntegerArgument<InternalSchemaMinLengthMatchExpression>(
                name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_LENGTH: {
            return parseInternalSchemaSingleIntegerArgument<InternalSchemaMaxLengthMatchExpression>(
                name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX: {
            return parseInternalSchemaMatchArrayIndex(
                name, e, expCtx, extensionsCallback, allowedFeatures, currentLevel);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX: {
            if (e.type() != BSONType::Array) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << " must be an array");
            }
            auto elemMatchObj = e.embeddedObject();
            auto iter = BSONObjIterator(elemMatchObj);
            if (!iter.more()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << " must be an array of size 2");
            }
            auto first = iter.next();
            auto parsedIndex = first.parseIntegerElementToNonNegativeLong();
            if (!parsedIndex.isOK()) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "first element of "
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << " must be a non-negative integer");
            }
            if (!iter.more()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << " must be an array of size 2");
            }
            auto second = iter.next();
            if (iter.more()) {
                return Status(ErrorCodes::FailedToParse,
                              str::stream()
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << " has too many elements, must be an array of size 2");
            }
            if (second.type() != BSONType::Object) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << "second element of "
                                  << InternalSchemaAllElemMatchFromIndexMatchExpression::kName
                                  << "must be an object");
            }

            auto filter = parse(second.embeddedObject(),
                                expCtx,
                                extensionsCallback,
                                MatchExpressionParser::kBanAllSpecialFeatures,
                                DocumentParseLevel::kUserSubDocument);

            if (!filter.isOK()) {
                return filter.getStatus();
            }

            auto exprWithPlaceholder =
                ExpressionWithPlaceholder::make(std::move(filter.getValue()));
            if (!exprWithPlaceholder.isOK()) {
                return exprWithPlaceholder.getStatus();
            }
            return {stdx::make_unique<InternalSchemaAllElemMatchFromIndexMatchExpression>(
                name, parsedIndex.getValue(), std::move(exprWithPlaceholder.getValue()))};
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_TYPE: {
            return parseType<InternalSchemaTypeExpression>(name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_EQ: {
            return {stdx::make_unique<InternalSchemaEqMatchExpression>(name, e)};
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE: {
            return parseType<InternalSchemaBinDataEncryptedTypeExpression>(name, e);
        }

        case PathAcceptingKeyword::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE: {
            return parseInternalSchemaBinDataSubType(name, e);
        }
    }

    return {
        Status(ErrorCodes::BadValue, str::stream() << "not handled: " << e.fieldNameStringData())};
}

/**
 * Parses a field in a sub expression.
 * If the query is { x : { $gt : 5, $lt : 8 } },
 * 'e' is { $gt : 5, $lt : 8 }
 */
Status parseSub(StringData name,
                const BSONObj& sub,
                AndMatchExpression* root,
                const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const ExtensionsCallback* extensionsCallback,
                MatchExpressionParser::AllowedFeatureSet allowedFeatures,
                DocumentParseLevel currentLevel) {
    // The one exception to {field : {fully contained argument} } is, of course, geo.  Example:
    // sub == { field : {$near[Sphere]: [0,0], $maxDistance: 1000, $minDistance: 10 } }
    // We peek inside of 'sub' to see if it's possibly a $near.  If so, we can't iterate over its
    // subfields and parse them one at a time (there is no $maxDistance without $near), so we hand
    // the entire object over to the geo parsing routines.

    // Special case parsing for geoNear. This is necessary in order to support query formats like
    // {$near: <coords>, $maxDistance: <distance>}. No other query operators allow $-prefixed
    // modifiers as sibling BSON elements.
    BSONObjIterator geoIt(sub);
    if (geoIt.more()) {
        auto firstElt = geoIt.next();
        if (firstElt.isABSONObj()) {
            if (MatchExpressionParser::parsePathAcceptingKeyword(firstElt) ==
                PathAcceptingKeyword::GEO_NEAR) {
                auto s =
                    parseGeo(name, PathAcceptingKeyword::GEO_NEAR, sub, expCtx, allowedFeatures);
                if (s.isOK()) {
                    root->add(s.getValue().release());
                }

                // Propagate geo parsing result to caller.
                return s.getStatus();
            }
        }
    }

    for (auto deep : sub) {
        auto s = parseSubField(
            sub, root, name, deep, expCtx, extensionsCallback, allowedFeatures, currentLevel);
        if (!s.isOK())
            return s.getStatus();

        expCtx->incrementMatchExprCounter(deep.fieldNameStringData());
        if (s.getValue())
            root->add(s.getValue().release());
    }

    return Status::OK();
}

}  // namespace

StatusWithMatchExpression MatchExpressionParser::parse(
    const BSONObj& obj,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ExtensionsCallback& extensionsCallback,
    AllowedFeatureSet allowedFeatures) {
    invariant(expCtx.get());
    const DocumentParseLevel currentLevelCall = DocumentParseLevel::kPredicateTopLevel;
    try {
        return ::mongo::parse(obj, expCtx, &extensionsCallback, allowedFeatures, currentLevelCall);
    } catch (const DBException& ex) {
        return {ex.toStatus()};
    }
}

namespace {
// Maps from query operator string name to function.
std::unique_ptr<StringMap<
    stdx::function<StatusWithMatchExpression(StringData,
                                             BSONElement,
                                             const boost::intrusive_ptr<ExpressionContext>&,
                                             const ExtensionsCallback*,
                                             MatchExpressionParser::AllowedFeatureSet,
                                             DocumentParseLevel)>>>
    pathlessOperatorMap;

MONGO_INITIALIZER(PathlessOperatorMap)(InitializerContext* context) {
    pathlessOperatorMap = stdx::make_unique<StringMap<
        stdx::function<StatusWithMatchExpression(StringData,
                                                 BSONElement,
                                                 const boost::intrusive_ptr<ExpressionContext>&,
                                                 const ExtensionsCallback*,
                                                 MatchExpressionParser::AllowedFeatureSet,
                                                 DocumentParseLevel)>>>(
        StringMap<
            stdx::function<StatusWithMatchExpression(StringData,
                                                     BSONElement,
                                                     const boost::intrusive_ptr<ExpressionContext>&,
                                                     const ExtensionsCallback*,
                                                     MatchExpressionParser::AllowedFeatureSet,
                                                     DocumentParseLevel)>>{
            {"_internalSchemaAllowedProperties", &parseInternalSchemaAllowedProperties},
            {"_internalSchemaCond",
             &parseInternalSchemaFixedArityArgument<InternalSchemaCondMatchExpression>},
            {"_internalSchemaMaxProperties",
             &parseTopLevelInternalSchemaSingleIntegerArgument<
                 InternalSchemaMaxPropertiesMatchExpression>},
            {"_internalSchemaMinProperties",
             &parseTopLevelInternalSchemaSingleIntegerArgument<
                 InternalSchemaMinPropertiesMatchExpression>},
            {"_internalSchemaRootDocEq", &parseInternalSchemaRootDocEq},
            {"_internalSchemaXor", &parseTreeTopLevel<InternalSchemaXorMatchExpression>},
            {"alwaysFalse", &parseAlwaysBoolean<AlwaysFalseMatchExpression>},
            {"alwaysTrue", &parseAlwaysBoolean<AlwaysTrueMatchExpression>},
            {"and", &parseTreeTopLevel<AndMatchExpression>},
            {"comment", &parseComment},
            {"db", &parseDBRef},
            {"expr", &parseExpr},
            {"id", &parseDBRef},
            {"jsonSchema", &parseJSONSchema},
            {"nor", &parseTreeTopLevel<NorMatchExpression>},
            {"or", &parseTreeTopLevel<OrMatchExpression>},
            {"ref", &parseDBRef},
            {"text", &parseText},
            {"where", &parseWhere},
        });
    return Status::OK();
}

// Maps from query operator string name to operator PathAcceptingKeyword.
std::unique_ptr<StringMap<PathAcceptingKeyword>> queryOperatorMap;

MONGO_INITIALIZER(MatchExpressionParser)(InitializerContext* context) {
    queryOperatorMap =
        stdx::make_unique<StringMap<PathAcceptingKeyword>>(StringMap<PathAcceptingKeyword>{
            // TODO: SERVER-19565 Add $eq after auditing callers.
            {"_internalExprEq", PathAcceptingKeyword::INTERNAL_EXPR_EQ},
            {"_internalSchemaAllElemMatchFromIndex",
             PathAcceptingKeyword::INTERNAL_SCHEMA_ALL_ELEM_MATCH_FROM_INDEX},
            {"_internalSchemaBinDataEncryptedType",
             PathAcceptingKeyword::INTERNAL_SCHEMA_BIN_DATA_ENCRYPTED_TYPE},
            {"_internalSchemaBinDataSubType",
             PathAcceptingKeyword::INTERNAL_SCHEMA_BIN_DATA_SUBTYPE},
            {"_internalSchemaEq", PathAcceptingKeyword::INTERNAL_SCHEMA_EQ},
            {"_internalSchemaFmod", PathAcceptingKeyword::INTERNAL_SCHEMA_FMOD},
            {"_internalSchemaMatchArrayIndex",
             PathAcceptingKeyword::INTERNAL_SCHEMA_MATCH_ARRAY_INDEX},
            {"_internalSchemaMaxItems", PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_ITEMS},
            {"_internalSchemaMaxLength", PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_LENGTH},
            {"_internalSchemaMinItems", PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_ITEMS},
            {"_internalSchemaMinItems", PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_ITEMS},
            {"_internalSchemaMinLength", PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_LENGTH},
            {"_internalSchemaObjectMatch", PathAcceptingKeyword::INTERNAL_SCHEMA_OBJECT_MATCH},
            {"_internalSchemaType", PathAcceptingKeyword::INTERNAL_SCHEMA_TYPE},
            {"_internalSchemaUniqueItems", PathAcceptingKeyword::INTERNAL_SCHEMA_UNIQUE_ITEMS},
            {"all", PathAcceptingKeyword::ALL},
            {"bitsAllClear", PathAcceptingKeyword::BITS_ALL_CLEAR},
            {"bitsAllSet", PathAcceptingKeyword::BITS_ALL_SET},
            {"bitsAnyClear", PathAcceptingKeyword::BITS_ANY_CLEAR},
            {"bitsAnySet", PathAcceptingKeyword::BITS_ANY_SET},
            {"elemMatch", PathAcceptingKeyword::ELEM_MATCH},
            {"exists", PathAcceptingKeyword::EXISTS},
            {"geoIntersects", PathAcceptingKeyword::GEO_INTERSECTS},
            {"geoNear", PathAcceptingKeyword::GEO_NEAR},
            {"geoWithin", PathAcceptingKeyword::WITHIN},
            {"gt", PathAcceptingKeyword::GREATER_THAN},
            {"gte", PathAcceptingKeyword::GREATER_THAN_OR_EQUAL},
            {"in", PathAcceptingKeyword::IN_EXPR},
            {"lt", PathAcceptingKeyword::LESS_THAN},
            {"lte", PathAcceptingKeyword::LESS_THAN_OR_EQUAL},
            {"mod", PathAcceptingKeyword::MOD},
            {"ne", PathAcceptingKeyword::NOT_EQUAL},
            {"near", PathAcceptingKeyword::GEO_NEAR},
            {"nearSphere", PathAcceptingKeyword::GEO_NEAR},
            {"nin", PathAcceptingKeyword::NOT_IN},
            {"options", PathAcceptingKeyword::OPTIONS},
            {"regex", PathAcceptingKeyword::REGEX},
            {"size", PathAcceptingKeyword::SIZE},
            {"type", PathAcceptingKeyword::TYPE},
            {"within", PathAcceptingKeyword::WITHIN},
        });
    return Status::OK();
}

/**
 * Returns the proper parser for the indicated pathless operator. Returns 'null' if 'name'
 * doesn't represent a known type.
 */
stdx::function<StatusWithMatchExpression(StringData,
                                         BSONElement,
                                         const boost::intrusive_ptr<ExpressionContext>&,
                                         const ExtensionsCallback*,
                                         MatchExpressionParser::AllowedFeatureSet,
                                         DocumentParseLevel)>
retrievePathlessParser(StringData name) {
    auto func = pathlessOperatorMap->find(name);
    if (func == pathlessOperatorMap->end()) {
        return nullptr;
    }
    return func->second;
}

MONGO_INITIALIZER_WITH_PREREQUISITES(MatchExpressionCounters,
                                     ("PathlessOperatorMap", "MatchExpressionParser"))
(InitializerContext* context) {
    static const std::set<std::string> exceptionsSet{"within",   // deprecated
                                                     "geoNear",  // aggregation stage
                                                     "db",       // $-prefixed field names
                                                     "id",
                                                     "ref",
                                                     "options"};

    for (auto&& [name, keyword] : *queryOperatorMap) {
        if (name[0] == '_' || exceptionsSet.count(name) > 0) {
            continue;
        }
        operatorCountersMatchExpressions.addMatchExprCounter("$" + name);
    }
    for (auto&& [name, fn] : *pathlessOperatorMap) {
        if (name[0] == '_' || exceptionsSet.count(name) > 0) {
            continue;
        }
        operatorCountersMatchExpressions.addMatchExprCounter("$" + name);
    }
    operatorCountersMatchExpressions.addMatchExprCounter("$not");
    operatorCountersMatchExpressions.addMatchExprCounter("$eq");
    return Status::OK();
}


}  // namespace

boost::optional<PathAcceptingKeyword> MatchExpressionParser::parsePathAcceptingKeyword(
    BSONElement typeElem, boost::optional<PathAcceptingKeyword> defaultKeyword) {
    auto fieldName = typeElem.fieldNameStringData();
    if (fieldName[0] == '$' && fieldName[1]) {
        auto opName = fieldName.substr(1);
        auto queryOp = queryOperatorMap->find(opName);

        if (queryOp == queryOperatorMap->end()) {
            return defaultKeyword;
        }
        return queryOp->second;
    }
    return defaultKeyword;
}
}  // namespace mongo
