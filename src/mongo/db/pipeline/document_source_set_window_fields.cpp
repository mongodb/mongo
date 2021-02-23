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

#include "mongo/db/pipeline/document_source_add_fields.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/db/pipeline/document_source_set_window_fields_gen.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/util/visit_helper.h"

using boost::intrusive_ptr;
using boost::optional;
using std::list;
using SortPatternPart = mongo::SortPattern::SortPatternPart;

namespace mongo {

REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(
    setWindowFields,
    LiteParsedDocumentSourceDefault::parse,
    document_source_set_window_fields::createFromBson,
    LiteParsedDocumentSource::AllowedWithApiStrict::kAlways,
    boost::none,
    ::mongo::feature_flags::gFeatureFlagWindowFunctions.isEnabledAndIgnoreFCV());

REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(
    _internalSetWindowFields,
    LiteParsedDocumentSourceDefault::parse,
    DocumentSourceInternalSetWindowFields::createFromBson,
    LiteParsedDocumentSource::AllowedWithApiStrict::kInternal,
    boost::none,
    ::mongo::feature_flags::gFeatureFlagWindowFunctions.isEnabledAndIgnoreFCV());

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
    uassert(5397906, "partitionBy field not yet supported", !partitionBy);

    optional<SortPattern> sortBy;
    if (auto sortSpec = spec.getSortBy()) {
        sortBy.emplace(*sortSpec, expCtx);
    }

    std::vector<WindowFunctionStatement> outputFields;
    for (auto&& outputElem : spec.getOutput()) {
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
            str::stream() << "The field '" << elem.fieldName()
                          << "' must be a window-function object",
            elem.type() == BSONType::Object && elem.Obj().nFields() == 1);
    return WindowFunctionStatement(
        elem.fieldName(),
        window_function::Expression::parse(elem.Obj().firstElement(), sortBy, expCtx));
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
    //     {$setWindowFields: {partitionBy: {$foo: "$x"}, sortBy: {y: 1}, fields: {...}}}

    // We move the partitionBy expression out into its own $set stage:
    //     {$set: {__tmp: {$foo: "$x"}}}
    //     {$setWindowFields: {partitionBy: "$__tmp", sortBy: {y: 1}, fields: {...}}}
    //     {$unset: '__tmp'}

    // This lets us insert a $sort in between:
    //     {$set: {__tmp: {$foo: "$x"}}}
    //     {$sort: {__tmp: 1, y: 1}}
    //     {$setWindowFields: {partitionBy: "$__tmp", sortBy: {y: 1}, fields: {...}}}
    //     {$unset: '__tmp'}

    // Which lets us replace $setWindowFields with $_internalSetWindowFields:
    //     {$set: {__tmp: {$foo: "$x"}}}
    //     {$sort: {__tmp: 1, y: 1}}
    //     {$_internalSetWindowFields: {partitionBy: "$__tmp", sortBy: {y: 1}, fields: {...}}}
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
    // If there is no partitionBy, both are empty.
    // If partitionBy is already a field path, we only fill in simplePartitionBy.
    // If partitionBy is a more complex expression, we will need to generate a $set stage,
    // which will bind the value of the expression to the name in simplePartitionBy.
    if (partitionBy) {
        auto exprFieldPath = dynamic_cast<ExpressionFieldPath*>(partitionBy->get());
        if (exprFieldPath && exprFieldPath->isRootFieldPath()) {
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
    if (simplePartitionBy || sortBy) {
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
        result.push_back(DocumentSourceSort::create(expCtx, SortPattern{combined}));
    }

    // $_internalSetWindowFields
    result.push_back(make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx, simplePartitionByExpr, sortBy, outputFields));

    // $unset
    if (complexPartitionBy) {
        result.push_back(DocumentSourceProject::createUnset(*simplePartitionBy, expCtx));
    }

    return result;
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

    return Value(DOC(kStageName << spec.freeze()));
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
        expCtx, partitionBy, sortBy, outputFields);
}

void DocumentSourceInternalSetWindowFields::initialize() {
    for (auto& wfs : _outputFields) {
        uassert(5397900, "Window function must be $sum", wfs.expr->getOpName() == "$sum");
        // TODO: SERVER-54340 Remove this check.
        uassert(5397905,
                "Window functions cannot set to dotted paths",
                wfs.fieldName.find('.') == std::string::npos);
        auto windowBounds = wfs.expr->bounds();
        stdx::visit(
            visit_helper::Overloaded{
                [](const WindowBounds::DocumentBased& docBase) {
                    stdx::visit(
                        visit_helper::Overloaded{
                            [](const WindowBounds::Unbounded) { /* pass */ },
                            [](auto&& other) {
                                uasserted(5397904,
                                          "Only 'unbounded' lower bound is currently supported");
                            }},
                        docBase.lower);
                    stdx::visit(
                        visit_helper::Overloaded{
                            [](const WindowBounds::Current) { /* pass */ },
                            [](auto&& other) {
                                uasserted(5397903,
                                          "Only 'current' upper bound is currently supported");
                            }},
                        docBase.upper);
                },
                [](const WindowBounds::RangeBased& rangeBase) {
                    uasserted(5397901, "Ranged based windows not currently supported");
                },
                [](const WindowBounds::TimeBased& timeBase) {
                    uasserted(5397902, "Time based windows are not currently supported");
                }},
            windowBounds.bounds);
        _executableOutputs.push_back(ExecutableWindowFunction(
            wfs.fieldName, AccumulatorSum::create(pExpCtx.get()), windowBounds, wfs.expr->input()));
    }
    _init = true;
}

DocumentSource::GetNextResult DocumentSourceInternalSetWindowFields::doGetNext() {
    if (!_init) {
        initialize();
    }

    auto curStat = pSource->getNext();
    if (!curStat.isAdvanced()) {
        return curStat;
    }
    auto curDoc = curStat.getDocument();
    if (_partitionBy) {
        uassert(ErrorCodes::TypeMismatch,
                "Cannot 'partitionBy' an expression of type array",
                !_partitionBy->get()->evaluate(curDoc, &pExpCtx->variables).isArray());
    }
    MutableDocument outDoc(curDoc);
    for (auto& output : _executableOutputs) {
        // Currently only support unbounded windows and run on the merging shard -- we don't need
        // to reset accumulators, merge states, or partition into multiple groups.
        output.accumulator->process(output.inputExpr->evaluate(curDoc, &pExpCtx->variables), false);
        outDoc.setNestedField(output.fieldName, output.accumulator->getValue(false));
    }
    return outDoc.freeze();
}

}  // namespace mongo
