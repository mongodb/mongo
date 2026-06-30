/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/extension/host/aggregation_stage/ast_node.h"

#include "mongo/bson/bson_validate.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_internal_document_results_and_metadata.h"
#include "mongo/db/pipeline/search/lite_parsed_internal_search_id_lookup.h"
#include "mongo/util/assert_util.h"

namespace mongo::extension::host {

// Shared control block for DPLCallbackOwner. Runs the deleter exactly once when the last shared
// copy is destroyed, and caches the single invocation's parsed output.
struct DPLCallbackOwner::State {
    State(CallbackInvoker invoker, std::function<void()> deleter)
        : invoker(std::move(invoker)), deleter(std::move(deleter)) {}
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
        if (deleter) {
            deleter();
        }
    }

    CallbackInvoker invoker;
    std::function<void()> deleter;
    bool invoked = false;
    DocumentSourceInternalDocumentResultsAndMetadata::ShardedPlanSpec result;
};

DPLCallbackOwner::DPLCallbackOwner(CallbackInvoker invoker, std::function<void()> deleter)
    : _state((invoker || deleter) ? std::make_shared<State>(std::move(invoker), std::move(deleter))
                                  : nullptr) {}

bool DPLCallbackOwner::hasCallback() const {
    return _state && _state->invoker;
}

const DocumentSourceInternalDocumentResultsAndMetadata::ShardedPlanSpec&
DPLCallbackOwner::getOrInvoke(ExpressionContext* expCtx) const {
    tassert(12728604, "getOrInvoke() called without a DPL callback", hasCallback());
    tassert(12728605, "getOrInvoke() requires a non-null ExpressionContext", expCtx != nullptr);
    auto& state = *_state;
    if (state.invoked) {
        return state.result;
    }
    // Mark invoked up front so the single-use extension callback is never re-entered, even if the
    // call or a later validation step throws.
    state.invoked = true;

    ::MongoExtensionByteBuf* rawSort = nullptr;
    ::MongoExtensionByteBuf* rawMerge = nullptr;
    // Declared before the invocation so the output buffers are owned (and freed) even if status
    // conversion throws after the callback has already allocated them.
    ExtensionByteBufHandle sortHandle(nullptr);
    ExtensionByteBufHandle mergeHandle(nullptr);
    invokeCAndConvertStatusToException([&] {
        auto* status = state.invoker(expCtx, &rawSort, &rawMerge);
        sortHandle = ExtensionByteBufHandle(rawSort);
        mergeHandle = ExtensionByteBufHandle(rawMerge);
        return status;
    });

    // bsonObjFromByteView only checks the outer length; validate the interior so a malformed
    // extension buffer yields a clean error rather than an out-of-bounds read during traversal.
    const auto ownedValidatedBson = [](const ::MongoExtensionByteView& view) {
        BSONObj obj = bsonObjFromByteView(view).getOwned();
        uassertStatusOK(validateBSON(obj.objdata(), obj.objsize()));
        return obj;
    };

    if (rawSort) {
        state.result.setResultsSortPattern(ownedValidatedBson(sortHandle->getByteView()));
    }
    if (rawMerge) {
        BSONObj arr = ownedValidatedBson(mergeHandle->getByteView());
        std::vector<BSONObj> pipeline;
        for (auto&& elem : arr) {
            uassert(12728601,
                    "DPL metaMergePipeline elements must be objects",
                    elem.type() == BSONType::object);
            pipeline.push_back(elem.Obj().getOwned());
        }
        state.result.setMetaMergePipeline(std::move(pipeline));
    }
    return state.result;
}

std::unique_ptr<LiteParsedDocumentSource> AggStageAstNode::expandToLiteParsed(
    const NamespaceString& nss, const LiteParserOptions& options) const {
    auto lpds = LiteParsedDocumentSource::parse(nss, buildStageBson(), options);
    lpds->makeOwned();
    return lpds;
}

std::list<boost::intrusive_ptr<DocumentSource>> AggStageAstNode::expandToDocumentSource(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    return DocumentSource::parse(expCtx, buildStageBson());
}

IdLookupAstNode::IdLookupAstNode(std::unique_ptr<LiteParsedInternalSearchIdLookUp> lp)
    : _liteParsed(std::move(lp)) {}

const std::string& IdLookupAstNode::getName() const {
    return _liteParsed->getParseTimeName();
}

BSONObj IdLookupAstNode::buildStageBson() const {
    return BSON(getName() << _liteParsed->getSpec().toBSON());
}

std::unique_ptr<AggStageAstNode> IdLookupAstNode::clone() const {
    return std::make_unique<IdLookupAstNode>(
        std::make_unique<LiteParsedInternalSearchIdLookUp>(_liteParsed->getSpec()));
}

DocumentResultsAndMetadataAstNode::DocumentResultsAndMetadataAstNode(BSONObj stageBson,
                                                                     DPLCallbackOwner dplOwner)
    : _stageName(stageBson.firstElementFieldName()),
      _stageBson(stageBson.getOwned()),
      _dplOwner(std::move(dplOwner)) {}

const std::string& DocumentResultsAndMetadataAstNode::getName() const {
    return _stageName;
}

BSONObj DocumentResultsAndMetadataAstNode::buildStageBson() const {
    return _stageBson;
}

std::unique_ptr<AggStageAstNode> DocumentResultsAndMetadataAstNode::clone() const {
    // Share the single-use DPL callback owner with the clone. The shared control block guarantees
    // the callback is invoked, and destroyed, only once, so cloning never silently drops the
    // callback regardless of which node is ultimately expanded.
    return std::make_unique<DocumentResultsAndMetadataAstNode>(_stageBson, _dplOwner);
}

DocResultsShardedPlanProvider DocumentResultsAndMetadataAstNode::makeShardedPlanProvider() const {
    if (!_dplOwner.hasCallback()) {
        return {};
    }
    // Capture a copy of the shared owner so it (and its cached single invocation) outlives this
    // AST node, which is destroyed after expansion but before the planner queries
    // distributedPlanLogic().
    return [owner = _dplOwner](ExpressionContext* expCtx) -> const auto& {
        return owner.getOrInvoke(expCtx);
    };
}

std::unique_ptr<LiteParsedDocumentSource> DocumentResultsAndMetadataAstNode::expandToLiteParsed(
    const NamespaceString& nss, const LiteParserOptions& options) const {
    auto lpds = AggStageAstNode::expandToLiteParsed(nss, options);
    auto provider = makeShardedPlanProvider();
    if (!provider) {
        return lpds;
    }
    // Carry the provider on the LiteParsed object so it survives the LiteParsed -> StageParams ->
    // DocumentSource handoff used by the LiteParsedDesugarer expansion path.
    auto* drmLp = dynamic_cast<InternalDocumentResultsAndMetadataLiteParsed*>(lpds.get());
    tassert(12878900,
            "Expected InternalDocumentResultsAndMetadataLiteParsed from AggStageAstNode expansion",
            drmLp != nullptr);
    drmLp->setShardedPlan(std::move(provider));
    return lpds;
}

std::list<boost::intrusive_ptr<DocumentSource>>
DocumentResultsAndMetadataAstNode::expandToDocumentSource(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    // Parse through the normal StageParams path, then inject the node's DPL callback onto the
    // resulting stage. The opaque callback cannot survive the BSON round-trip, so it is attached
    // here rather than threaded through a bespoke BSON factory.
    auto stages = AggStageAstNode::expandToDocumentSource(expCtx);
    auto provider = makeShardedPlanProvider();
    if (!provider) {
        return stages;
    }
    tassert(12728602,
            "$_internalDocumentResultsAndMetadata must expand to exactly one stage",
            stages.size() == 1);
    auto* drm =
        dynamic_cast<DocumentSourceInternalDocumentResultsAndMetadata*>(stages.front().get());
    tassert(12728603,
            "expanded $_internalDocumentResultsAndMetadata stage has unexpected type",
            drm != nullptr);
    drm->setShardedPlan(std::move(provider));
    return stages;
}

}  // namespace mongo::extension::host
