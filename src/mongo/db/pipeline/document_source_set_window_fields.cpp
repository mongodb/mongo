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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/add_fields_projection_executor.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_set_window_fields_gen.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/sort_pattern.h"
#include "mongo/util/visit_helper.h"

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using SortPatternPart = mongo::SortPattern::SortPatternPart;

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

list<intrusive_ptr<DocumentSource>> document_source_set_window_fields::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    auto spec =
        SetWindowFieldsSpec::parse(IDLParserErrorContext(kStageName), elem.embeddedObject());
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

    return create(
        std::move(expCtx), std::move(partitionBy), std::move(sortBy), std::move(outputFields));
}

WindowFunctionStatement WindowFunctionStatement::parse(BSONElement elem,
                                                       const boost::optional<SortPattern>& sortBy,
                                                       ExpressionContext* expCtx) {
    // 'elem' is a statement like 'v: {$sum: {...}}', whereas the expression is '$sum: {...}'.
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The field '" << elem.fieldName() << "' must be an object",
            elem.type() == BSONType::Object);
    return WindowFunctionStatement(
        elem.fieldName(),
        window_function::Expression::parse(elem.embeddedObject(), sortBy, expCtx));
}
void WindowFunctionStatement::serialize(MutableDocument& outputFields,
                                        boost::optional<ExplainOptions::Verbosity> explain) const {
    outputFields[fieldName] = expr->serialize(explain);
}

list<intrusive_ptr<DocumentSource>> document_source_set_window_fields::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    optional<intrusive_ptr<Expression>> partitionBy,
    const optional<SortPattern>& sortBy,
    std::vector<WindowFunctionStatement> outputFields) {

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
        try {
            partitionBy = (*partitionBy)->optimize();
        } catch (DBException& ex) {
            ex.addContext("Failed to optimize partitionBy expression");
            throw;
        }
        if (auto exprConst = dynamic_cast<ExpressionConstant*>(partitionBy->get())) {
            uassert(ErrorCodes::TypeMismatch,
                    "An expression used to partition cannot evaluate to value of type array",
                    !exprConst->getValue().isArray());
            // Partitioning by a constant, non-array expression is equivalent to not partitioning
            // (putting everything in the same partition).
        } else if (auto exprFieldPath = dynamic_cast<ExpressionFieldPath*>(partitionBy->get());
                   exprFieldPath && !exprFieldPath->isVariableReference()) {
            // ExpressionFieldPath has "CURRENT" as an explicit first component,
            // but for $sort we don't want that.
            simplePartitionBy = exprFieldPath->getFieldPath().tail();
            simplePartitionByExpr = partitionBy;
        } else {
            // In DocumentSource we don't have a mechanism for generating non-colliding field names,
            // so we have to choose the tmp name carefully to make a collision unlikely in practice.
            std::array<unsigned char, 16> nonce = UUID::gen().data();
            // We encode as a base64 string for a shorter, more performant field name (length 22).
            std::string tmpField = base64::encode(nonce.data(), sizeof(nonce));
            simplePartitionBy = FieldPath{tmpField};
            simplePartitionByExpr = ExpressionFieldPath::createPathFromString(
                expCtx.get(), tmpField, expCtx->variablesParseState);
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
        for (auto part : *sortBy) {
            combined.push_back(part);
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
        result.push_back(DocumentSourceSort::create(expCtx, SortPattern{combined}));
    }

    // $_internalSetWindowFields
    result.push_back(make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx,
        simplePartitionByExpr,
        sortBy,
        outputFields,
        internalDocumentSourceSetWindowFieldsMaxMemoryBytes.load()));

    // $unset
    if (complexPartitionBy) {
        result.push_back(DocumentSourceProject::createUnset(*simplePartitionBy, expCtx));
    }

    return result;
}

intrusive_ptr<DocumentSource> DocumentSourceInternalSetWindowFields::optimize() {
    // The _partitionBy is already optimized in create(), along with _iterator which initializes
    // with it. The _executableOutputs will be constructed using the expressions from the
    // '_outputFields' on the first call to doGetNext(). As a result, only expressions in the
    // '_outputFeilds' are optimized here.
    for (auto&& outputField : _outputFields) {
        outputField.expr->optimize();
    }
    return this;
}

Value DocumentSourceInternalSetWindowFields::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    MutableDocument spec;
    spec[SetWindowFieldsSpec::kPartitionByFieldName] =
        _partitionBy ? (*_partitionBy)->serialize(false) : Value();

    auto sortKeySerialization = explain
        ? SortPattern::SortKeySerialization::kForExplain
        : SortPattern::SortKeySerialization::kForPipelineSerialization;
    spec[SetWindowFieldsSpec::kSortByFieldName] =
        _sortBy ? Value(_sortBy->serialize(sortKeySerialization)) : Value();

    MutableDocument output;
    for (auto&& stmt : _outputFields) {
        stmt.serialize(output, explain);
    }
    spec[SetWindowFieldsSpec::kOutputFieldName] = output.freezeToValue();

    MutableDocument out;
    out[getSourceName()] = Value(spec.freeze());

    if (explain && *explain >= ExplainOptions::Verbosity::kExecStats) {
        MutableDocument md;

        for (auto&& [fieldName, function] : _executableOutputs) {
            md[fieldName] =
                Value(static_cast<long long>(_memoryTracker[fieldName].maxMemoryBytes()));
        }

        out["maxFunctionMemoryUsageBytes"] = Value(md.freezeToValue());
        out["maxTotalMemoryUsageBytes"] =
            Value(static_cast<long long>(_memoryTracker.maxMemoryBytes()));
        out["usedDisk"] = Value(_iterator.usedDisk());
    }

    return Value(out.freezeToValue());
}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalSetWindowFields::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "the " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::Object);

    auto spec =
        SetWindowFieldsSpec::parse(IDLParserErrorContext(kStageName), elem.embeddedObject());
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

    std::vector<WindowFunctionStatement> outputFields;
    for (auto&& elem : spec.getOutput()) {
        outputFields.push_back(WindowFunctionStatement::parse(elem, sortBy, expCtx.get()));
    }

    return make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx,
        partitionBy,
        sortBy,
        outputFields,
        internalDocumentSourceSetWindowFieldsMaxMemoryBytes.load());
}

void DocumentSourceInternalSetWindowFields::initialize() {
    for (auto& wfs : _outputFields) {
        _executableOutputs[wfs.fieldName] =
            WindowFunctionExec::create(pExpCtx.get(), &_iterator, wfs, _sortBy, &_memoryTracker);
    }
    _init = true;
}

Pipeline::SourceContainer::iterator DocumentSourceInternalSetWindowFields::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
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

DocumentSource::GetNextResult DocumentSourceInternalSetWindowFields::doGetNext() {
    if (!_init) {
        initialize();
    }

    if (_eof)
        return DocumentSource::GetNextResult::makeEOF();

    auto curDoc = _iterator.current();
    // The only way we hit this case is if there are no documents, since otherwise _eof will be set.
    if (!curDoc) {
        _eof = true;
        return DocumentSource::GetNextResult::makeEOF();
    }

    // Populate the output document with the result from each window function.
    auto projSpec = std::make_unique<projection_executor::InclusionNode>(
        ProjectionPolicies{ProjectionPolicies::DefaultIdPolicy::kIncludeId});
    for (auto&& [fieldName, function] : _executableOutputs) {
        try {
            // If we hit a uassert while evaluating expressions on user data, delete the temporary
            // table before aborting the operation.
            projSpec->addExpressionForPath(
                FieldPath(fieldName),
                ExpressionConstant::create(pExpCtx.get(), function->getNext()));
        } catch (const DBException&) {
            _iterator.finalize();
            throw;
        }

        if (_memoryTracker.currentMemoryBytes() >=
                static_cast<long long>(_memoryTracker._maxAllowedMemoryUsageBytes) &&
            _memoryTracker._allowDiskUse) {
            // Attempt to spill where possible.
            _iterator.spillToDisk();
        }
        if (_memoryTracker.currentMemoryBytes() >
            static_cast<long long>(_memoryTracker._maxAllowedMemoryUsageBytes)) {
            _iterator.finalize();
            uasserted(5414201,
                      str::stream()
                          << "Exceeded memory limit in DocumentSourceSetWindowFields, used "
                          << _memoryTracker.currentMemoryBytes() << " bytes but max allowed is "
                          << _memoryTracker._maxAllowedMemoryUsageBytes);
        }
    }

    // Advance the iterator and handle partition/EOF edge cases.
    switch (_iterator.advance()) {
        case PartitionIterator::AdvanceResult::kAdvanced:
            break;
        case PartitionIterator::AdvanceResult::kNewPartition:
            // We've advanced to a new partition, reset the state of every function as well as the
            // memory tracker.
            _memoryTracker.resetCurrent();
            for (auto&& [fieldName, function] : _executableOutputs) {
                function->reset();
            }

            // Account for the memory in the iterator for the new partition.
            _memoryTracker.set(_iterator.getApproximateSize());
            break;
        case PartitionIterator::AdvanceResult::kEOF:
            _eof = true;
            _iterator.finalize();
            break;
    }

    // Avoid using the factory 'create' on the executor since we don't want to re-parse.
    auto projExec = std::make_unique<projection_executor::AddFieldsProjectionExecutor>(
        pExpCtx, std::move(projSpec));

    return projExec->applyProjection(*curDoc);
}

}  // namespace mongo
