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

using boost::intrusive_ptr;
using boost::optional;
using std::list;

namespace mongo {

REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(
    setWindowFields,
    LiteParsedDocumentSourceDefault::parse,
    document_source_set_window_fields::createFromBson,
    boost::none,
    ::mongo::feature_flags::gFeatureFlagWindowFunctions.isEnabledAndIgnoreFCV());

REGISTER_DOCUMENT_SOURCE_CONDITIONALLY(
    _internalSetWindowFields,
    LiteParsedDocumentSourceDefault::parse,
    DocumentSourceInternalSetWindowFields::createFromBson,
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
    return create(std::move(expCtx),
                  std::move(partitionBy),
                  std::move(spec.getSortBy()),
                  std::move(spec.getOutput()));
}

list<intrusive_ptr<DocumentSource>> document_source_set_window_fields::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    optional<intrusive_ptr<Expression>> partitionBy,
    optional<BSONObj> sortBy,
    BSONObj fields) {

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
    if (simplePartitionBy || sortBy) {
        BSONObjBuilder sortSpec;
        if (simplePartitionBy) {
            sortSpec << simplePartitionBy->fullPath() << 1;
        }
        if (sortBy) {
            for (auto elem : *sortBy) {
                sortSpec << elem;
            }
        }
        result.push_back(DocumentSourceSort::create(expCtx, sortSpec.obj()));
    }

    // $_internalSetWindowFields
    result.push_back(make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx, simplePartitionByExpr, sortBy, fields));

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
    spec[SetWindowFieldsSpec::kSortByFieldName] = _sortBy ? Value(*_sortBy) : Value();
    spec[SetWindowFieldsSpec::kOutputFieldName] = Value(_fields);
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
    return make_intrusive<DocumentSourceInternalSetWindowFields>(
        expCtx, partitionBy, spec.getSortBy(), spec.getOutput());
}

DocumentSource::GetNextResult DocumentSourceInternalSetWindowFields::doGetNext() {
    // This is a placeholder: it returns every input doc unchanged.
    return pSource->getNext();
}

}  // namespace mongo
