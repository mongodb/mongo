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

#include "mongo/db/update/pipeline_executor.h"

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/update/object_replace_executor.h"
#include "mongo/db/update/storage_validation.h"

namespace mongo {

namespace {
constexpr StringData kIdFieldName = "_id"_sd;
}  // namespace

PipelineExecutor::PipelineExecutor(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                   const std::vector<BSONObj>& pipeline,
                                   boost::optional<BSONObj> constants)
    : _expCtx(expCtx) {
    // "Resolve" involved namespaces into a map. We have to populate this map so that any
    // $lookups, etc. will not fail instantiation. They will not be used for execution as these
    // stages are not allowed within an update context.
    AggregationRequest aggRequest(NamespaceString("dummy.namespace"), pipeline);
    LiteParsedPipeline liteParsedPipeline(aggRequest);
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    for (auto&& nss : liteParsedPipeline.getInvolvedNamespaces()) {
        resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
    }

    if (constants) {
        for (auto&& constElem : *constants) {
            const auto constName = constElem.fieldNameStringData();
            Variables::uassertValidNameForUserRead(constName);

            auto varId = _expCtx->variablesParseState.defineVariable(constName);
            _expCtx->variables.setConstantValue(varId, Value(constElem));
        }
    }

    _expCtx->setResolvedNamespaces(resolvedNamespaces);
    _pipeline = uassertStatusOK(Pipeline::parse(pipeline, _expCtx));

    // Validate the update pipeline.
    for (auto&& stage : _pipeline->getSources()) {
        auto stageConstraints = stage->constraints();
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << stage->getSourceName()
                              << " is not allowed to be used within an update",
                stageConstraints.isAllowedWithinUpdatePipeline);

        invariant(stageConstraints.requiredPosition ==
                  StageConstraints::PositionRequirement::kNone);
        invariant(!stageConstraints.isIndependentOfAnyCollection);
    }

    _pipeline->addInitialSource(DocumentSourceQueue::create(expCtx));
}

UpdateExecutor::ApplyResult PipelineExecutor::applyUpdate(ApplyParams applyParams) const {
    DocumentSourceQueue* queueStage = static_cast<DocumentSourceQueue*>(_pipeline->peekFront());
    queueStage->emplace_back(Document{applyParams.element.getDocument().getObject()});
    auto transformedDoc = _pipeline->getNext()->toBson();
    auto transformedDocHasIdField = transformedDoc.hasField(kIdFieldName);

    return ObjectReplaceExecutor::applyReplacementUpdate(
        applyParams, transformedDoc, transformedDocHasIdField);
}

Value PipelineExecutor::serialize() const {
    std::vector<Value> valueArray;
    for (const auto& stage : _pipeline->getSources()) {
        // The queue stage we add to adapt the pull-based '_pipeline' to our use case should not
        // be serialized out. Firstly, this was not part of the user's pipeline and is just an
        // implementation detail. It wouldn't have much value in exposing. Secondly, supporting
        // a serialization that we can later re-parse is non trivial. See the comment in
        // DocumentSourceQueue for more details.
        if (typeid(*stage) == typeid(DocumentSourceQueue)) {
            continue;
        }

        stage->serializeToArray(valueArray);
    }

    return Value(valueArray);
}

}  // namespace mongo
