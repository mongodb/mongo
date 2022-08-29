/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/pipeline/abt/match_expression_visitor.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/expression_where_noop.h"
#include "mongo/db/matcher/match_expression_walker.h"
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
#include "mongo/db/pipeline/abt/agg_expression_visitor.h"
#include "mongo/db/pipeline/abt/expr_algebrizer_context.h"
#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

class ABTMatchExpressionVisitor : public MatchExpressionConstVisitor {
public:
    ABTMatchExpressionVisitor(ExpressionAlgebrizerContext& ctx, const bool allowAggExpressions)
        : _allowAggExpressions(allowAggExpressions), _ctx(ctx) {}

    void visit(const AlwaysFalseMatchExpression* expr) override {
        generateBoolConstant(false);
    }

    void visit(const AlwaysTrueMatchExpression* expr) override {
        generateBoolConstant(true);
    }

    void visit(const AndMatchExpression* expr) override {
        visitAndOrExpression<PathComposeM, true>(expr);
    }

    void visit(const BitsAllClearMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const BitsAllSetMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const BitsAnyClearMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const BitsAnySetMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const ElemMatchObjectMatchExpression* expr) override {
        generateElemMatch<false /*isValueElemMatch*/>(expr);
    }

    void visit(const ElemMatchValueMatchExpression* expr) override {
        generateElemMatch<true /*isValueElemMatch*/>(expr);
    }

    void visit(const EqualityMatchExpression* expr) override {
        generateSimpleComparison(expr, Operations::Eq);
    }

    void visit(const ExistsMatchExpression* expr) override {
        assertSupportedPathExpression(expr);

        ABT result = make<PathDefault>(Constant::boolean(false));
        if (!expr->path().empty()) {
            result = generateFieldPath(FieldPath(expr->path().toString()), std::move(result));
        }
        _ctx.push(std::move(result));
    }

    void visit(const ExprMatchExpression* expr) override {
        uassert(6624246, "Cannot generate an agg expression in this context", _allowAggExpressions);

        ABT result = generateAggExpression(
            expr->getExpression().get(), _ctx.getRootProjection(), _ctx.getUniqueIdPrefix());

        if (auto filterPtr = result.cast<EvalFilter>();
            filterPtr != nullptr && filterPtr->getInput() == _ctx.getRootProjVar()) {
            // If we have an EvalFilter, just return the path.
            _ctx.push(std::move(filterPtr->getPath()));
        } else {
            _ctx.push<PathConstant>(std::move(result));
        }
    }

    void visit(const GTEMatchExpression* expr) override {
        generateSimpleComparison(expr, Operations::Gte);
    }

    void visit(const GTMatchExpression* expr) override {
        generateSimpleComparison(expr, Operations::Gt);
    }

    void visit(const GeoMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const GeoNearMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InMatchExpression* expr) override {
        uassert(ErrorCodes::InternalErrorNotSupported,
                "$in with regexes is not supported.",
                expr->getRegexes().empty());

        assertSupportedPathExpression(expr);

        const auto& equalities = expr->getEqualities();

        // $in with an empty equalities list matches nothing; replace with constant false.
        if (equalities.empty()) {
            generateBoolConstant(false);
            return;
        }

        ABT result = make<PathIdentity>();

        const auto [tagTraverse, valTraverse] = sbe::value::makeNewArray();
        sbe::value::ValueGuard arrGuard{tagTraverse, valTraverse};
        auto arrTraversePtr = sbe::value::getArrayView(valTraverse);
        arrTraversePtr->reserve(equalities.size());

        const auto [tagArraysOnly, valArraysOnly] = sbe::value::makeNewArray();
        sbe::value::ValueGuard arrOnlyGuard{tagArraysOnly, valArraysOnly};
        auto arraysOnlyPtr = sbe::value::getArrayView(valArraysOnly);
        bool addNullPathDefault = false;
        arraysOnlyPtr->reserve(equalities.size());

        for (const auto& pred : equalities) {
            const auto [tag, val] = convertFrom(Value(pred));
            arrTraversePtr->push_back(tag, val);

            if (tag == sbe::value::TypeTags::Null) {
                addNullPathDefault = true;
            } else if (tag == sbe::value::TypeTags::Array) {
                const auto [tag2, val2] = sbe::value::copyValue(tag, val);
                arraysOnlyPtr->push_back(tag2, val2);
            }
        }

        // If there is only one term in the match expression, we don't need to use EqMember
        if (arrTraversePtr->size() == 1) {
            const auto [tagSingle, valSingle] = sbe::value::copyValue(
                arrTraversePtr->getAt(0).first, arrTraversePtr->getAt(0).second);
            result = make<PathCompare>(Operations::Eq, make<Constant>(tagSingle, valSingle));
        } else {
            result =
                make<PathCompare>(Operations::EqMember, make<Constant>(tagTraverse, valTraverse));
            arrGuard.reset();
        }

        if (addNullPathDefault) {
            maybeComposePath<PathComposeA>(result, make<PathDefault>(Constant::boolean(true)));
        }

        // The path can be empty if we are within an $elemMatch. In this case elemMatch would
        // insert a traverse.
        if (!expr->path().empty()) {
            // When the path we are comparing is a path to an array, the comparison is
            // considered true if it evaluates to true for the array itself or for any of the
            // array’s elements. 'result' evaluates comparison on the array elements, and
            // 'arraysOnly' evaluates the comparison on the array itself.
            result = make<PathTraverse>(std::move(result), PathTraverse::kSingleLevel);

            if (arraysOnlyPtr->size() == 1) {
                const auto [tagSingle, valSingle] = sbe::value::copyValue(
                    arraysOnlyPtr->getAt(0).first, arraysOnlyPtr->getAt(0).second);
                maybeComposePath<PathComposeA>(
                    result,
                    make<PathCompare>(Operations::Eq, make<Constant>(tagSingle, valSingle)));
            } else if (arraysOnlyPtr->size() > 0) {
                maybeComposePath<PathComposeA>(
                    result,
                    make<PathCompare>(Operations::EqMember,
                                      make<Constant>(tagArraysOnly, valArraysOnly)));
                arrOnlyGuard.reset();
            }
            result = generateFieldPath(FieldPath(expr->path().toString()), std::move(result));
        }
        _ctx.push(std::move(result));
    }

    void visit(const InternalBucketGeoWithinMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprEqMatchExpression* expr) override {
        // Ignored. Translate to "true".
        _ctx.push(make<PathConstant>(Constant::boolean(true)));
    }

    void visit(const InternalExprGTMatchExpression* expr) override {
        // Ignored. Translate to "true".
        _ctx.push(make<PathConstant>(Constant::boolean(true)));
    }

    void visit(const InternalExprGTEMatchExpression* expr) override {
        // Ignored. Translate to "true".
        _ctx.push(make<PathConstant>(Constant::boolean(true)));
    }

    void visit(const InternalExprLTMatchExpression* expr) override {
        // Ignored. Translate to "true".
        _ctx.push(make<PathConstant>(Constant::boolean(true)));
    }

    void visit(const InternalExprLTEMatchExpression* expr) override {
        // Ignored. Translate to "true".
        _ctx.push(make<PathConstant>(Constant::boolean(true)));
    }

    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaBinDataSubTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaCondMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaEqMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaFmodMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMaxItemsMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMaxLengthMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMinItemsMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMinLengthMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaObjectMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaRootDocEqMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaXorMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const LTEMatchExpression* expr) override {
        generateSimpleComparison(expr, Operations::Lte);
    }

    void visit(const LTMatchExpression* expr) override {
        generateSimpleComparison(expr, Operations::Lt);
    }

    void visit(const ModMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const NorMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const NotMatchExpression* expr) override {
        ABT result = _ctx.pop();
        _ctx.push(make<PathConstant>(make<UnaryOp>(
            Operations::Not,
            make<EvalFilter>(std::move(result), make<Variable>(_ctx.getRootProjection())))));
    }

    void visit(const OrMatchExpression* expr) override {
        visitAndOrExpression<PathComposeA, false>(expr);
    }

    void visit(const RegexMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const SizeMatchExpression* expr) override {
        assertSupportedPathExpression(expr);

        const std::string lambdaProjName = _ctx.getNextId("lambda_sizeMatch");
        ABT result = make<PathLambda>(make<LambdaAbstraction>(
            lambdaProjName,
            make<BinaryOp>(
                Operations::Eq,
                make<FunctionCall>("getArraySize", makeSeq(make<Variable>(lambdaProjName))),
                Constant::int64(expr->getData()))));

        if (!expr->path().empty()) {
            // No traverse.
            result = translateFieldPath(
                FieldPath(expr->path().toString()),
                std::move(result),
                [](const std::string& fieldName, const bool /*isLastElement*/, ABT input) {
                    return make<PathGet>(fieldName, std::move(input));
                });
        }
        _ctx.push(std::move(result));
    }

    void visit(const TextMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TextNoOpMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TwoDPtInAnnulusExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TypeMatchExpression* expr) override {
        assertSupportedPathExpression(expr);

        const std::string lambdaProjName = _ctx.getNextId("lambda_typeMatch");
        ABT result = make<PathLambda>(make<LambdaAbstraction>(
            lambdaProjName,
            make<FunctionCall>("typeMatch",
                               makeSeq(make<Variable>(lambdaProjName),
                                       Constant::int32(expr->typeSet().getBSONTypeMask())))));

        // The path can be empty if we are within an $elemMatch. In this case elemMatch would insert
        // a traverse.
        if (!expr->path().empty()) {
            result = make<PathTraverse>(std::move(result), PathTraverse::kSingleLevel);
            if (expr->typeSet().hasType(BSONType::Array)) {
                // If we are testing against array type, insert a comparison against the
                // non-traversed path (the array itself if we have one).
                result = make<PathComposeA>(make<PathArr>(), std::move(result));
            }

            result = generateFieldPath(FieldPath(expr->path().toString()), std::move(result));
        }
        _ctx.push(std::move(result));
    }

    void visit(const WhereMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const WhereNoOpMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const EncryptedBetweenMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

private:
    void generateBoolConstant(const bool value) {
        _ctx.push<PathConstant>(Constant::boolean(value));
    }

    template <bool isValueElemMatch>
    void generateElemMatch(const ArrayMatchingMatchExpression* expr) {
        assertSupportedPathExpression(expr);

        // Returns true if at least one sub-objects matches the condition.

        const size_t childCount = expr->numChildren();
        if (childCount == 0) {
            _ctx.push(Constant::boolean(true));
        }

        _ctx.ensureArity(childCount);
        ABT result = _ctx.pop();
        for (size_t i = 1; i < childCount; i++) {
            maybeComposePath(result, _ctx.pop());
        }
        if constexpr (!isValueElemMatch) {
            // Make sure we consider only objects or arrays as elements of the array.
            maybeComposePath(result, make<PathComposeA>(make<PathObj>(), make<PathArr>()));
        }
        result = make<PathTraverse>(std::move(result), PathTraverse::kSingleLevel);

        // Make sure we consider only arrays fields on the path.
        maybeComposePath(result, make<PathArr>());

        if (!expr->path().empty()) {
            result = translateFieldPath(
                FieldPath{expr->path().toString()},
                std::move(result),
                [&](const std::string& fieldName, const bool isLastElement, ABT input) {
                    if (!isLastElement) {
                        input = make<PathTraverse>(std::move(input), PathTraverse::kSingleLevel);
                    }
                    return make<PathGet>(fieldName, std::move(input));
                });
        }

        _ctx.push(std::move(result));
    }

    /**
     * Return the minimum or maximum value for the "class" of values represented by the input
     * constant. Used to support type bracketing.
     * Return format is <min/max value, bool inclusive>
     */
    template <bool isMin>
    std::pair<boost::optional<ABT>, bool> getMinMaxBoundForType(const sbe::value::TypeTags& tag) {
        switch (tag) {
            case sbe::value::TypeTags::NumberInt32:
            case sbe::value::TypeTags::NumberInt64:
            case sbe::value::TypeTags::NumberDouble:
            case sbe::value::TypeTags::NumberDecimal:
                if constexpr (isMin)
                    return {Constant::fromDouble(std::numeric_limits<double>::quiet_NaN()), true};
                else
                    return {Constant::str(""), false};
            case sbe::value::TypeTags::StringSmall:
            case sbe::value::TypeTags::StringBig:
            case sbe::value::TypeTags::bsonString:
            case sbe::value::TypeTags::bsonSymbol:
                if constexpr (isMin)
                    return {Constant::str(""), true};
                else {
                    return {Constant::emptyObject(), false};
                }
            case sbe::value::TypeTags::Date:
                if constexpr (isMin)
                    return {Constant::date(Date_t::min()), true};
                else
                    return {Constant::date(Date_t::max()), true};
            case sbe::value::TypeTags::Timestamp:
                if constexpr (isMin)
                    return {Constant::timestamp(Timestamp::min()), true};
                else
                    return {Constant::timestamp(Timestamp::max()), true};
            case sbe::value::TypeTags::Null:
                return {Constant::null(), true};
            case sbe::value::TypeTags::bsonUndefined: {
                auto [tag, val] = convertFrom(mongo::Value(BSONUndefined));
                return {make<Constant>(sbe::value::TypeTags::bsonUndefined, val), true};
            }
            case sbe::value::TypeTags::Object:
            case sbe::value::TypeTags::bsonObject:
                if constexpr (isMin)
                    return {Constant::emptyObject(), true};
                else
                    return {Constant::emptyArray(), false};
            case sbe::value::TypeTags::Array:
            case sbe::value::TypeTags::ArraySet:
            case sbe::value::TypeTags::bsonArray:
                if constexpr (isMin) {
                    return {Constant::emptyArray(), true};
                } else {
                    auto [tag, val] = mongo::optimizer::convertFrom(mongo::Value(BSONBinData()));
                    return {make<Constant>(sbe::value::TypeTags::bsonBinData, val), false};
                }
            case sbe::value::TypeTags::bsonBinData:
                if constexpr (isMin) {
                    auto [tag, val] = mongo::optimizer::convertFrom(mongo::Value(BSONBinData()));
                    return {make<Constant>(sbe::value::TypeTags::bsonBinData, val), true};
                } else {
                    auto [tag, val] = mongo::optimizer::convertFrom(mongo::Value(OID()));
                    return {make<Constant>(sbe::value::TypeTags::ObjectId, val), false};
                }
            case sbe::value::TypeTags::Boolean:
                if constexpr (isMin)
                    return {Constant::boolean(false), true};
                else
                    return {Constant::boolean(true), true};
            case sbe::value::TypeTags::ObjectId:
            case sbe::value::TypeTags::bsonObjectId:
                if constexpr (isMin) {
                    auto [tag, val] = mongo::optimizer::convertFrom(mongo::Value(OID()));
                    return {make<Constant>(sbe::value::TypeTags::ObjectId, val), true};
                } else {
                    auto [tag, val] = mongo::optimizer::convertFrom(mongo::Value(OID::max()));
                    return {make<Constant>(sbe::value::TypeTags::ObjectId, val), true};
                }
            case sbe::value::TypeTags::bsonRegex:
                if constexpr (isMin) {
                    auto [tag, val] = convertFrom(mongo::Value(BSONRegEx("", "")));
                    return {make<Constant>(sbe::value::TypeTags::bsonRegex, val), true};
                } else {
                    auto [tag, val] = convertFrom(mongo::Value(mongo::BSONDBRef()));
                    return {make<Constant>(sbe::value::TypeTags::bsonDBPointer, val), false};
                }
            case sbe::value::TypeTags::bsonDBPointer:
                if constexpr (isMin) {
                    auto [tag, val] = convertFrom(mongo::Value(mongo::BSONDBRef()));
                    return {make<Constant>(sbe::value::TypeTags::bsonDBPointer, val), true};
                } else {
                    auto [tag, val] = sbe::value::makeCopyBsonJavascript(StringData(""));
                    return {make<Constant>(sbe::value::TypeTags::bsonJavascript, val), false};
                }
            case sbe::value::TypeTags::bsonJavascript:
                if constexpr (isMin) {
                    auto [tag, val] = sbe::value::makeCopyBsonJavascript(StringData(""));
                    return {make<Constant>(sbe::value::TypeTags::bsonJavascript, val), true};
                } else {
                    auto bsonCode = BSONCodeWScope();
                    auto [tag, val] = convertFrom(mongo::Value(bsonCode));
                    return {make<Constant>(sbe::value::TypeTags::bsonCodeWScope, val), false};
                }
            case sbe::value::TypeTags::bsonCodeWScope:
                if constexpr (isMin) {
                    auto bsonCode = BSONCodeWScope();
                    auto [tag, val] = convertFrom(mongo::Value(bsonCode));
                    return {make<Constant>(sbe::value::TypeTags::bsonCodeWScope, val), true};
                } else {
                    return {Constant::maxKey(), false};
                }
            default:
                return {boost::none, false};
        }
        MONGO_UNREACHABLE;
    }

    ABT generateFieldPath(const FieldPath& fieldPath, ABT initial) {
        return translateFieldPath(
            fieldPath,
            std::move(initial),
            [&](const std::string& fieldName, const bool isLastElement, ABT input) {
                if (!isLastElement) {
                    input = make<PathTraverse>(std::move(input), PathTraverse::kSingleLevel);
                }
                return make<PathGet>(fieldName, std::move(input));
            });
    }

    void assertSupportedPathExpression(const PathMatchExpression* expr) {
        uassert(ErrorCodes::InternalErrorNotSupported,
                "Expression contains a numeric path component",
                !FieldRef(expr->path()).hasNumericPathComponents());
    }

    void generateSimpleComparison(const ComparisonMatchExpressionBase* expr, const Operations op) {
        assertSupportedPathExpression(expr);

        auto [tag, val] = convertFrom(Value(expr->getData()));
        ABT result = make<PathCompare>(op, make<Constant>(tag, val));

        switch (op) {
            case Operations::Lt:
            case Operations::Lte: {
                auto&& [constant, inclusive] = getMinMaxBoundForType<true /*isMin*/>(tag);
                if (constant) {
                    maybeComposePath(result,
                                     make<PathCompare>(inclusive ? Operations::Gte : Operations::Gt,
                                                       std::move(constant.get())));
                }
                // Handle null and missing semantics
                // find({a: {$lt: MaxKey()}}) matches {a: null} and {b: 1}
                if (tag == sbe::value::TypeTags::MaxKey) {
                    maybeComposePath<PathComposeA>(result,
                                                   make<PathDefault>(Constant::boolean(true)));
                }
                break;
            }

            case Operations::Gt:
            case Operations::Gte: {
                auto&& [constant, inclusive] = getMinMaxBoundForType<false /*isMin*/>(tag);
                if (constant) {
                    maybeComposePath(result,
                                     make<PathCompare>(inclusive ? Operations::Lte : Operations::Lt,
                                                       std::move(constant.get())));
                }
                // Handle null and missing semantics
                // find({a: {$gt: MinKey()}}) matches {a: null} and {b: 1}
                if (tag == sbe::value::TypeTags::MinKey) {
                    maybeComposePath<PathComposeA>(result,
                                                   make<PathDefault>(Constant::boolean(true)));
                }
                break;
            }

            case Operations::Eq: {
                if (tag == sbe::value::TypeTags::Null) {
                    // Handle null and missing semantics. Matching against null also implies
                    // matching against missing.
                    result = make<PathComposeA>(make<PathDefault>(Constant::boolean(true)),
                                                std::move(result));
                }
                break;
            }

            default:
                break;
        }

        // The path can be empty if we are within an $elemMatch. In this case elemMatch would
        // insert a traverse.
        if (!expr->path().empty()) {
            if (tag == sbe::value::TypeTags::Array || tag == sbe::value::TypeTags::MinKey ||
                tag == sbe::value::TypeTags::MaxKey) {
                // The behavior of PathTraverse when it encounters an array is to apply its subpath
                // to every element of the array and not the array itself. When an expression is
                // comparing a field to an array, minKey or maxKey constant, we need to ensure that
                // these comparisons happen to every element of the array and the array itself.
                // For example:
                // find({a: [1]}) matches {a: [1]} and {a: [[1]]}
                // find({a: {$gt: MinKey()}}) matches {a: []}
                result = make<PathComposeA>(make<PathTraverse>(result, PathTraverse::kSingleLevel),
                                            result);
            } else {
                result = make<PathTraverse>(std::move(result), PathTraverse::kSingleLevel);
            }

            result = generateFieldPath(FieldPath(expr->path().toString()), std::move(result));
        }
        _ctx.push(std::move(result));
    }

    template <class Composition, bool defaultResult>
    void visitAndOrExpression(const ListOfMatchExpression* expr) {
        const size_t childCount = expr->numChildren();
        if (childCount == 0) {
            generateBoolConstant(defaultResult);
            return;
        }
        if (childCount == 1) {
            return;
        }

        ABTVector nodes;
        for (size_t i = 0; i < childCount; i++) {
            nodes.push_back(_ctx.pop());
        }

        // Construct a balanced composition tree.
        maybeComposePaths<Composition>(nodes);
        _ctx.push(std::move(nodes.front()));
    }

    void unsupportedExpression(const MatchExpression* expr) const {
        uasserted(ErrorCodes::InternalErrorNotSupported,
                  str::stream() << "Match expression is not supported: " << expr->matchType());
    }

    // If we are parsing a partial index filter, we don't allow agg expressions.
    const bool _allowAggExpressions;

    // We don't own this
    ExpressionAlgebrizerContext& _ctx;
};

ABT generateMatchExpression(const MatchExpression* expr,
                            const bool allowAggExpressions,
                            const std::string& rootProjection,
                            const std::string& uniqueIdPrefix) {
    ExpressionAlgebrizerContext ctx(
        false /*assertExprSort*/, true /*assertPathSort*/, rootProjection, uniqueIdPrefix);
    ABTMatchExpressionVisitor visitor(ctx, allowAggExpressions);
    MatchExpressionWalker walker(nullptr /*preVisitor*/, nullptr /*inVisitor*/, &visitor);
    tree_walker::walk<true, MatchExpression>(expr, &walker);
    return ctx.pop();
}

}  // namespace mongo::optimizer
