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
#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_union_with.h"
#include "mongo/db/pipeline/document_source_union_with_gen.h"

namespace mongo {

REGISTER_TEST_DOCUMENT_SOURCE(unionWith,
                              DocumentSourceUnionWith::LiteParsed::parse,
                              DocumentSourceUnionWith::createFromBson);

std::unique_ptr<DocumentSourceUnionWith::LiteParsed> DocumentSourceUnionWith::LiteParsed::parse(
    const AggregationRequest& request, const BSONElement& spec) {
    uassert(ErrorCodes::FailedToParse,
            str::stream()
                << "the $unionWith stage specification must be an object or string, but found "
                << typeName(spec.type()),
            spec.type() == BSONType::Object || spec.type() == BSONType::String);

    NamespaceString unionNss;
    stdx::unordered_set<NamespaceString> foreignNssSet;
    boost::optional<LiteParsedPipeline> liteParsedPipeline;
    if (spec.type() == BSONType::String) {
        unionNss = NamespaceString(request.getNamespaceString().db(), spec.valueStringData());
    } else {
        auto unionWithSpec =
            UnionWithSpec::parse(IDLParserErrorContext(kStageName), spec.embeddedObject());
        unionNss = NamespaceString(request.getNamespaceString().db(), unionWithSpec.getColl());

        // Recursively lite parse the nested pipeline, if one exists.
        if (unionWithSpec.getPipeline()) {
            AggregationRequest foreignAggReq(unionNss, std::move(*unionWithSpec.getPipeline()));
            liteParsedPipeline = LiteParsedPipeline(foreignAggReq);
            foreignNssSet.merge(liteParsedPipeline->getInvolvedNamespaces());
        }
    }

    foreignNssSet.insert(unionNss);

    return std::make_unique<DocumentSourceUnionWith::LiteParsed>(
        std::move(unionNss), std::move(foreignNssSet), std::move(liteParsedPipeline));
}

boost::intrusive_ptr<DocumentSource> DocumentSourceUnionWith::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream()
                << "the $unionWith stage specification must be an object or string, but found "
                << typeName(elem.type()),
            elem.type() == BSONType::Object || elem.type() == BSONType::String);

    NamespaceString unionNss;
    std::vector<BSONObj> pipeline;
    if (elem.type() == BSONType::String) {
        unionNss = NamespaceString(expCtx->ns.db().toString(), elem.valueStringData());
    } else {
        auto unionWithSpec =
            UnionWithSpec::parse(IDLParserErrorContext(kStageName), elem.embeddedObject());
        unionNss = NamespaceString(expCtx->ns.db().toString(), unionWithSpec.getColl());
        pipeline = unionWithSpec.getPipeline().value_or(std::vector<BSONObj>{});
    }
    return make_intrusive<DocumentSourceUnionWith>(
        expCtx,
        uassertStatusOK(
            Pipeline::parse(std::move(pipeline), expCtx->copyWith(std::move(unionNss)))));
}

DocumentSource::GetNextResult DocumentSourceUnionWith::doGetNext() {
    if (!_sourceExhausted) {
        auto nextInput = pSource->getNext();
        if (!nextInput.isEOF()) {
            return nextInput;
        }
        _sourceExhausted = true;
        // All documents from the base collection have been returned, switch to iterating the sub-
        // pipeline by falling through below.
    }

    if (!_cursorAttached) {
        auto ctx = _pipeline->getContext();
        _pipeline =
            pExpCtx->mongoProcessInterface->attachCursorSourceToPipeline(ctx, _pipeline.release());
        _cursorAttached = true;
    }

    if (auto res = _pipeline->getNext())
        return std::move(*res);

    return GetNextResult::makeEOF();
}

DocumentSource::GetModPathsReturn DocumentSourceUnionWith::getModifiedPaths() const {
    // Since we might have a document arrive from the foreign pipeline with the same path as a
    // document in the main pipeline. Without introspecting the sub-pipeline, we must report that
    // all paths have been modified.
    return {GetModPathsReturn::Type::kAllPaths, {}, {}};
}

void DocumentSourceUnionWith::doDispose() {
    if (_pipeline) {
        _pipeline->dispose(pExpCtx->opCtx);
        _pipeline.reset();
    }
}

void DocumentSourceUnionWith::serializeToArray(
    std::vector<Value>& array, boost::optional<ExplainOptions::Verbosity> explain) const {
    BSONArrayBuilder bab;
    for (auto&& stage : _pipeline->serialize())
        bab << stage;
    Document doc = DOC(getSourceName() << DOC("coll" << _pipeline->getContext()->ns.coll()
                                                     << "pipeline" << bab.arr()));
    array.push_back(Value(doc));
}

DepsTracker::State DocumentSourceUnionWith::getDependencies(DepsTracker* deps) const {
    // Since the $unionWith stage is a simple passthrough, we *could* report SEE_NEXT here in an
    // attempt to get a covered plan for the base collection. The ideal solution would involve
    // pushing down any dependencies to the inner pipeline as well.
    return DepsTracker::State::NOT_SUPPORTED;
}

void DocumentSourceUnionWith::detachFromOperationContext() {
    // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
    // use Pipeline::detachFromOperationContext() to take care of updating the Pipeline's
    // ExpressionContext.
    _pipeline->detachFromOperationContext();
}

void DocumentSourceUnionWith::reattachToOperationContext(OperationContext* opCtx) {
    // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
    // use Pipeline::reattachToOperationContext() to take care of updating the Pipeline's
    // ExpressionContext.
    _pipeline->reattachToOperationContext(opCtx);
}

void DocumentSourceUnionWith::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    collectionNames->insert(_pipeline->getContext()->ns);
    collectionNames->merge(_pipeline->getInvolvedCollections());
}

}  // namespace mongo
