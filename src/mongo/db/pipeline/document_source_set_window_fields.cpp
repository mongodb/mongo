/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <absl/container/flat_hash_map.h>
#include <boost/container/small_vector.hpp>
#include <boost/optional.hpp>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/exec/inclusion_projection_executor.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_set_window_fields_gen.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/window_function/window_function_exec.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/base64.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <iterator>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using SortPatternPart = mongo::SortPattern::SortPatternPart;

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {
/**
 * Does a sort pattern contain a path that has been modified?
 */
bool modifiedSortPaths(const SortPattern& pat, const DocumentSource::GetModPathsReturn& paths) {
    for (const auto& path : pat) {
        if (!path.fieldPath.has_value()) {
            return true;
        }
        auto sortFieldPath = path.fieldPath->fullPath();
        auto it = std::find_if(
            paths.paths.begin(), paths.paths.end(), [&sortFieldPath](const auto& modPath) {
                return sortFieldPath == modPath ||
                    expression::isPathPrefixOf(sortFieldPath, modPath) ||
                    expression::isPathPrefixOf(modPath, sortFieldPath);
            });
        if (it != paths.paths.end()) {
            return true;
        }
    }
    return false;
}
}  // namespace

REGISTER_DOCUMENT_SOURCE(setWindowFields,
                         LiteParsedDocumentSourceDefault::parse,
                         document_source_set_window_fields::createFromBson,
                         AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE(_internalSetWindowFields,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceInternalSetWindowFields::createFromBson,
                         AllowedWithApiStrict::kAlways);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalSetWindowFields, DocumentSourceInternalSetWindowFields::id)

list<intrusive_ptr<DocumentSource>> document_source_set_window_fields::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = SetWindowFieldsSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));
    auto partitionBy = [&]() -> boost::optional<boost::intrusive_ptr<Expression>> {
        if (auto partitionBy = spec.getPartitionBy())
            return Expression::parseOperand(
                expCtx.get(), partitionBy->getElement(), expCtx->variablesParseState);
        else
            return boost::none;
    }();

    optional<SortPattern> sortBy;
    if (auto sortSpec = spec.getSortBy()) {
        sortBy.emplace(*sortSpec, expCtx);
    }

    // Verify that the computed fields are valid and do not conflict with each other.
    FieldRefSet fieldSet;
    std::vector<FieldRef> backingRefs;

    expCtx->setSbeWindowCompatibility(SbeCompatibility::noRequirements);
    std::vector<WindowFunctionStatement> outputFields;
    const auto& output = spec.getOutput();
    backingRefs.reserve(output.nFields());
    for (auto&& outputElem : output) {
        backingRefs.push_back(FieldRef(outputElem.fieldNameStringData()));
        const FieldRef* conflict;
        uassert(6307900,
                "$setWindowFields 'output' specification contains two conflicting paths",
                fieldSet.insert(&backingRefs.back(), &conflict));
        outputFields.push_back(WindowFunctionStatement::parse(outputElem, sortBy, expCtx.get()));
    }
    auto sbeCompatibility =
        std::min(expCtx->getSbeWindowCompatibility(), expCtx->getSbeCompatibility());

    return create(expCtx,
                  std::move(partitionBy),
                  std::move(sortBy),
                  std::move(outputFields),
                  sbeCompatibility);
}

list<intrusive_ptr<DocumentSource>> document_source_set_window_fields::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    optional<intrusive_ptr<Expression>> partitionBy,
    optional<SortPattern> sortBy,
    std::vector<WindowFunctionStatement> outputFields,
    SbeCompatibility sbeCompatibility) {

    // Starting with an input like this:
    //     {$setWindowFields: {partitionBy: {$foo: "$x"}, sortBy: {y: 1}, output: {...}}}

    // We move the partitionBy expression out into its own $set stage:
    //     {$set: {__tmp: {$foo: "$x"}}}
    //     {$setWindowFields: {partitionBy: "$__tmp", sortBy: {y: 1}, output: {...}}}
    //     {$unset: '__tmp'}

    // This lets us insert a $sort in between:
    //     {$set: {__tmp: {$foo: "$x"}}}
    //     {$sort: {__tmp: 1, y: 1}}
    //     {$setWindowFields: {partitionBy: "$__tmp", sortBy: {y: 1}, output: {...}}}
    //     {$unset: '__tmp'}

    // Which lets us replace $setWindowFields with $_internalSetWindowFields:
    //     {$set: {__tmp: {$foo: "$x"}}}
    //     {$sort: {__tmp: 1, y: 1}}
    //     {$_internalSetWindowFields: {partitionBy: "$__tmp", sortBy: {y: 1}, output: {...}}}
    //     {$unset: '__tmp'}

    // If partitionBy is a field path, we can $sort by that field directly and avoid creating a
    // $set stage. This is important for pushing down the $sort. This is only valid because we
    // assert (in getNextInput()) that partitionBy is never an array.

    // If there is no partitionBy at all then we just $sort by the sortBy spec.

    // If there is no sortBy and no partitionBy then we can omit the $sort stage completely.

    list<intrusive_ptr<DocumentSource>> result;

    // complexPartitionBy is an expression to evaluate.
    // simplePartitionBy is a field path, which can be evaluated or sorted.
    optional<intrusive_ptr<Expression>> complexPartitionBy;
    optional<FieldPath> simplePartitionBy;
    optional<intrusive_ptr<Expression>> simplePartitionByExpr;
    // If partitionBy is a constant or there is no partitionBy, both are empty.
    // If partitionBy is already a field path, we only fill in simplePartitionBy.
    // If partitionBy is a more complex expression, we will need to generate a $set stage,
    // which will bind the value of the expression to the name in simplePartitionBy.
    if (partitionBy) {
        // Catch any failures that may surface during optimizing the partitionBy expression and add
        // context. This allows for the testing infrastructure to detect when parsing fails due to
        // a new optimization, which passed on an earlier version without the optimization.
        //
        // We can only safely call 'optimize' before 'doOptimizeAt' if there are no user defined
        // variables.
        // TODO SERVER-84113: we should be able to call optimize here or move the call to optimize
        // inside 'doOptimizeAt'.
        std::set<Variables::Id> refs;
        expression::addVariableRefs((*partitionBy).get(), &refs);
        if (!Variables::hasVariableReferenceTo(
                refs, expCtx->variablesParseState.getDefinedVariableIDs())) {
            try {
                partitionBy = (*partitionBy)->optimize();
            } catch (DBException& ex) {
                ex.addContext("Failed to optimize partitionBy expression");
                throw;
            }
        }
        if (auto exprConst = dynamic_cast<ExpressionConstant*>(partitionBy->get())) {
            uassert(ErrorCodes::TypeMismatch,
                    "An expression used to partition cannot evaluate to value of type array",
                    !exprConst->getValue().isArray());
            // Partitioning by a constant, non-array expression is equivalent to not partitioning
            // (putting everything in the same partition).
        } else if (auto exprFieldPath = dynamic_cast<ExpressionFieldPath*>(partitionBy->get());
                   exprFieldPath && !exprFieldPath->isVariableReference() &&
                   !exprFieldPath->isROOT()) {
            // ExpressionFieldPath has "CURRENT" as an explicit first component,
            // but for $sort we don't want that.
            simplePartitionBy = exprFieldPath->getFieldPath().tail();
            simplePartitionByExpr = partitionBy;
        } else {
            // In DocumentSource we don't have a mechanism for generating non-colliding field names,
            // so we have to choose the tmp name carefully to make a collision unlikely in practice.
            auto tmp = "__internal_setWindowFields_partition_key";
            simplePartitionBy = FieldPath{tmp};
            simplePartitionByExpr = ExpressionFieldPath::createPathFromString(
                expCtx.get(), tmp, expCtx->variablesParseState);
            complexPartitionBy = partitionBy;
        }
    }

    // $set
    if (complexPartitionBy) {
        result.push_back(
            DocumentSourceAddFields::create(*simplePartitionBy, *complexPartitionBy, expCtx));
    }

    // $sort
    // Generate a combined SortPattern for the partition key and sortBy.
    std::vector<SortPatternPart> combined;

    if (simplePartitionBy) {
        SortPatternPart part;
        part.fieldPath = simplePartitionBy->fullPath();
        combined.emplace_back(std::move(part));
    }
    if (sortBy) {
        for (const auto& part : *sortBy) {
            // Check and filter out the partition by field within the sort field. The partition
            // field is already added to the combined sort field variable. As the partition only
            // contains documents that have the same value in the partition key an additional sort
            // over that field does not change the order or the documents within the partition.
            // Hence a sort by $partion does not change the sort order.
            if (!simplePartitionBy || *simplePartitionBy != part.fieldPath) {
                combined.push_back(part);
            }
        }
    }

    // This is for our testing framework. If this knob is set we append an _id to the translated
    // sortBy in order to ensure deterministic output.
    if (internalQueryAppendIdToSetWindowFieldsSort.load()) {
        SortPatternPart part;
        part.fieldPath = "_id"_sd;
        combined.push_back(part);
    }

    if (!combined.empty()) {
        result.push_back(DocumentSourceSort::create(
            expCtx,
            SortPattern{std::move(combined)},
            // We will rely on this to efficiently compute ranks.
            {.outputSortKeyMetadata = expCtx->isBasicRankFusionEnabled()}));
    }

    // $_internalSetWindowFields
    result.push_back(make_intrusive<DocumentSourceInternalSetWindowFields>(expCtx,
                                                                           simplePartitionByExpr,
                                                                           std::move(sortBy),
                                                                           std::move(outputFields),
                                                                           sbeCompatibility));

    // $unset
    if (complexPartitionBy) {
        result.push_back(DocumentSourceProject::createUnset(*simplePartitionBy, expCtx));
    }

    return result;
}

intrusive_ptr<DocumentSource> DocumentSourceInternalSetWindowFields::optimize() {
    // '_partitionBy' might have not been optimized in create(). Therefore we must call optimize
    // just in case for '_partitionBy' and '_partitionExpr' in '_iterator'. The '_executableOutputs'
    // will be constructed using the expressions from the'_outputFields' on the first call to
    // doGetNext(). As a result, '_outputFields' are optimized here.
    if (_partitionBy) {
        _partitionBy = _partitionBy->get()->optimize();
    }

    if (_outputFields.size() > 0) {
        // Calculate the new expression SBE compatibility after optimization without overwriting
        // the previous SBE compatibility value. See the optimize() function for $group for a more
        // detailed explanation.
        auto expCtx = _outputFields[0].expr->expCtx();
        auto origSbeCompatibility = expCtx->getSbeCompatibility();
        expCtx->setSbeCompatibility(SbeCompatibility::noRequirements);

        for (auto&& outputField : _outputFields) {
            outputField.expr->optimize();
        }

        _sbeCompatibility = std::min(_sbeCompatibility, expCtx->getSbeCompatibility());
        expCtx->setSbeCompatibility(origSbeCompatibility);
    }
    return this;
}

Value DocumentSourceInternalSetWindowFields::serialize(const SerializationOptions& opts) const {
    MutableDocument spec;
    spec[SetWindowFieldsSpec::kPartitionByFieldName] =
        _partitionBy ? (*_partitionBy)->serialize(opts) : Value();

    auto sortKeySerialization = opts.verbosity
        ? SortPattern::SortKeySerialization::kForExplain
        : SortPattern::SortKeySerialization::kForPipelineSerialization;
    spec[SetWindowFieldsSpec::kSortByFieldName] =
        _sortBy ? Value(_sortBy->serialize(sortKeySerialization, opts)) : Value();

    MutableDocument output;
    for (auto&& stmt : _outputFields) {
        stmt.serialize(output, opts);
    }
    spec[SetWindowFieldsSpec::kOutputFieldName] = output.freezeToValue();

    MutableDocument out;
    out[getSourceName()] = Value(spec.freeze());

    return Value(out.freezeToValue());
}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalSetWindowFields::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = SetWindowFieldsSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));
    auto partitionBy = [&]() -> boost::optional<boost::intrusive_ptr<Expression>> {
        if (auto partitionBy = spec.getPartitionBy())
            return Expression::parseOperand(
                expCtx.get(), partitionBy->getElement(), expCtx->variablesParseState);
        else
            return boost::none;
    }();

    optional<SortPattern> sortBy;
    if (auto sortSpec = spec.getSortBy()) {
        sortBy.emplace(*sortSpec, expCtx);
    }

    expCtx->setSbeWindowCompatibility(SbeCompatibility::noRequirements);
    std::vector<WindowFunctionStatement> outputFields;
    for (auto&& elem : spec.getOutput()) {
        outputFields.push_back(WindowFunctionStatement::parse(elem, sortBy, expCtx.get()));
    }
    auto sbeCompatibility =
        std::min(expCtx->getSbeWindowCompatibility(), expCtx->getSbeCompatibility());

    return make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx, partitionBy, sortBy, outputFields, sbeCompatibility);
}

DocumentSourceContainer::iterator DocumentSourceInternalSetWindowFields::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

    if (itr == container->begin()) {
        return std::next(itr);
    }

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    auto nextSort = dynamic_cast<DocumentSourceSort*>((*std::next(itr)).get());
    auto prevSort = dynamic_cast<DocumentSourceSort*>((*std::prev(itr)).get());

    if (!nextSort || !prevSort) {
        return std::next(itr);
    }

    auto nextPattern = nextSort->getSortKeyPattern();
    auto prevPattern = prevSort->getSortKeyPattern();

    if (nextSort->getLimit() != boost::none || modifiedSortPaths(nextPattern, getModifiedPaths())) {
        return std::next(itr);
    }

    // Sort is redundant if prefix of _internalSetWindowFields' sort pattern.
    //
    // Ex.
    //
    // {$sort: {a: 1, b: 1}},
    // {$_internalSetWindowFields: _},
    // {$sort: {a: 1}}
    //
    // is equivalent to
    //
    // {$sort: {a: 1, b: 1}},
    // {$_internalSetWindowFields: _}
    //
    if (nextPattern.size() <= prevPattern.size()) {
        for (size_t i = 0; i < nextPattern.size(); i++) {
            if (nextPattern[i] != prevPattern[i]) {
                return std::next(itr);
            }
        }
        container->erase(std::next(itr));
        return itr;
    }

    // Push down if sort pattern contains _internalSetWindowFields' sort pattern.
    //
    // Ex.
    //
    //  {$sort: {a: 1, b: 1}},
    //  {$_internalSetWindowFields: _},
    //  {$sort: {a: 1, b: 1, c: 1}}
    //
    // is equivalent to
    //
    //  {$sort: {a: 1, b: 1}},
    //  {$sort: {a: 1, b: 1, c: 1}},
    //  {$_internalSetWindowFields: _}
    //
    for (size_t i = 0; i < prevPattern.size(); i++) {
        if (nextPattern[i] != prevPattern[i]) {
            return std::next(itr);
        }
    }

    // Swap the $_internalSetWindowFields with the following $sort.
    std::swap(*itr, *std::next(itr));
    // Now 'itr' is still valid but points to the $sort we pushed down.

    // We want to give other optimizations a chance to take advantage of the change:
    // 1. The previous sort can remove itself.
    // 2. Other stages may interact with the newly pushed down sort.
    // So we want to look at the stage *before* the previous sort, if any.
    itr = std::prev(itr);
    // Now 'itr' points to the previous sort.

    if (itr == container->begin())
        return itr;
    return std::prev(itr);
}

}  // namespace mongo
