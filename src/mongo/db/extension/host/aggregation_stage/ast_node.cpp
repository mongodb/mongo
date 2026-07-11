// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
            tassert(ErrorCodes::ExtensionError,
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
    tassert(ErrorCodes::ExtensionError,
            "$_internalDocumentResultsAndMetadata must expand to exactly one stage",
            stages.size() == 1);
    auto* drm =
        dynamic_cast<DocumentSourceInternalDocumentResultsAndMetadata*>(stages.front().get());
    tassert(ErrorCodes::ExtensionError,
            "expanded $_internalDocumentResultsAndMetadata stage has unexpected type",
            drm != nullptr);
    drm->setShardedPlan(std::move(provider));
    return stages;
}

}  // namespace mongo::extension::host
