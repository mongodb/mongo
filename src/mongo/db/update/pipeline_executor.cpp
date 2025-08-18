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

#include "mongo/db/update/pipeline_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/agg/queue_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variable_validation.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/object_replace_executor.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"

#include <list>
#include <typeinfo>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

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
    LiteParsedPipeline liteParsedPipeline(expCtx->getNamespaceString(), pipeline);
    ResolvedNamespaceMap resolvedNamespaces;
    for (const auto& nss : liteParsedPipeline.getInvolvedNamespaces()) {
        resolvedNamespaces.try_emplace(nss, nss, std::vector<BSONObj>{});
    }

    if (constants) {
        for (auto&& constElem : *constants) {
            const auto constName = constElem.fieldNameStringData();
            variableValidation::validateNameForUserRead(constName);

            auto varId = _expCtx->variablesParseState.defineVariable(constName);
            _expCtx->variables.setConstantValue(varId, Value(constElem));
        }
    }

    _expCtx->setResolvedNamespaces(resolvedNamespaces);
    _expCtx->startExpressionCounters();
    _pipeline = Pipeline::parse(pipeline, _expCtx);
    _expCtx->stopExpressionCounters();

    // Validate the update pipeline.
    for (const auto& stage : _pipeline->getSources()) {
        auto stageConstraints = stage->constraints();
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << stage->getSourceName()
                              << " is not allowed to be used within an update",
                stageConstraints.isAllowedWithinUpdatePipeline);

        invariant(stageConstraints.requiredPosition ==
                  StageConstraints::PositionRequirement::kNone);
        invariant(!stageConstraints.isIndependentOfAnyCollection);

        if (stageConstraints.checkExistenceForDiffInsertOperations) {
            _checkExistenceForDiffInsertOperations = true;
        }
    }

    _pipeline->addInitialSource(DocumentSourceQueue::create(expCtx));
    _execPipeline = exec::agg::buildPipeline(_pipeline->freeze());
}

UpdateExecutor::ApplyResult PipelineExecutor::applyUpdate(ApplyParams applyParams) const {
    const auto originalDoc = applyParams.element.getDocument().getObject();
    auto* queueStage =
        dynamic_cast<exec::agg::QueueStage*>(_execPipeline->getStages().front().get());
    tassert(10817001, "expected the first stage in the pipeline to be a QueueStage", queueStage);
    queueStage->emplace_back(Document{originalDoc});

    const auto transformedDoc = _execPipeline->getNext()->toBson();
    const auto transformedDocHasIdField = transformedDoc.hasField(kIdFieldName);

    // Replace the pre-image document in applyParams with the post image we got from running the
    // post image.
    auto ret =
        ObjectReplaceExecutor::applyReplacementUpdate(applyParams,
                                                      transformedDoc,
                                                      transformedDocHasIdField,
                                                      true /* allowTopLevelDollarPrefixedFields */);

    // The oplog entry should not have been populated yet.
    invariant(ret.oplogEntry.isEmpty());

    if (applyParams.logMode != ApplyParams::LogMode::kDoNotGenerateOplogEntry && !ret.noop) {
        if (applyParams.logMode == ApplyParams::LogMode::kGenerateOplogEntry) {
            // We're allowed to generate $v: 2 log entries. The $v:2 has certain meta-fields like
            // '$v', 'diff'. So we pad some additional byte while computing diff.
            const auto diff = doc_diff::computeOplogDiff(
                originalDoc, transformedDoc, update_oplog_entry::kSizeOfDeltaOplogEntryMetadata);
            if (diff) {
                ret.oplogEntry = update_oplog_entry::makeDeltaOplogEntry(*diff);
                return ret;
            }
        }

        // Either we can't use diffing or diffing failed so fall back to full replacement. Set the
        // replacement to the value set by the object replace executor, in case it changed _id or
        // anything like that.
        ret.oplogEntry = update_oplog_entry::makeReplacementOplogEntry(
            applyParams.element.getDocument().getObject());
    }

    return ret;
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
