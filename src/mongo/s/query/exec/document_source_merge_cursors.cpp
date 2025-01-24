/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/s/query/exec/document_source_merge_cursors.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <utility>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/query/exec/cluster_query_result.h"
#include "mongo/s/resource_yielders.h"
#include "mongo/util/assert_util.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(mergeCursors,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceMergeCursors::createFromBson,
                         AllowedWithApiStrict::kInternal);
ALLOCATE_DOCUMENT_SOURCE_ID(mergeCursors, DocumentSourceMergeCursors::id)

constexpr StringData DocumentSourceMergeCursors::kStageName;

DocumentSourceMergeCursors::DocumentSourceMergeCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    AsyncResultsMergerParams armParams,
    boost::optional<BSONObj> ownedParamsSpec)
    : DocumentSource(kStageName, expCtx),
      _armParamsObj(std::move(ownedParamsSpec)),
      _armParams(std::move(armParams)) {

    // Populate the shard ids from the 'RemoteCursor'.
    recordRemoteCursorShardIds(_armParams->getRemotes());
}

std::size_t DocumentSourceMergeCursors::getNumRemotes() const {
    if (_blockingResultsMerger) {
        return _blockingResultsMerger->getNumRemotes();
    }
    return _armParams->getRemotes().size();
}

BSONObj DocumentSourceMergeCursors::getHighWaterMark() {
    if (!_blockingResultsMerger) {
        populateMerger();
    }
    return _blockingResultsMerger->getHighWaterMark();
}

bool DocumentSourceMergeCursors::remotesExhausted() const {
    if (!_blockingResultsMerger) {
        // We haven't started iteration yet.
        return false;
    }
    return _blockingResultsMerger->remotesExhausted();
}

Status DocumentSourceMergeCursors::setAwaitDataTimeout(Milliseconds awaitDataTimeout) {
    if (!_blockingResultsMerger) {
        // In cases where a cursor was established with a batchSize of 0, the first getMore
        // might specify a custom maxTimeMS (AKA await data timeout). In these cases we will not
        // have iterated the cursor yet so will not have populated the merger, but need to
        // remember/track the custom await data timeout. We will soon iterate the cursor, so we
        // just populate the merger now and let it track the await data timeout itself.
        populateMerger();
    }
    return _blockingResultsMerger->setAwaitDataTimeout(awaitDataTimeout);
}

void DocumentSourceMergeCursors::addNewShardCursors(std::vector<RemoteCursor>&& newCursors) {
    tassert(9535000, "_blockingResultsMerger must be set", _blockingResultsMerger);
    recordRemoteCursorShardIds(newCursors);
    _blockingResultsMerger->addNewShardCursors(std::move(newCursors));
}

void DocumentSourceMergeCursors::closeShardCursors(const stdx::unordered_set<ShardId>& shardIds) {
    tassert(8456113, "_blockingResultsMerger must be set", _blockingResultsMerger);
    _blockingResultsMerger->closeShardCursors(shardIds);
}

void DocumentSourceMergeCursors::populateMerger() {
    tassert(9535001, "_blockingResultsMerger must not yet be set", !_blockingResultsMerger);
    tassert(9535002, "_armParams must be set", _armParams);

    _blockingResultsMerger.emplace(
        pExpCtx->getOperationContext(),
        std::move(*_armParams),
        pExpCtx->getMongoProcessInterface()->taskExecutor,
        // Assumes this is only called from the 'aggregate' or 'getMore' commands.  The code which
        // relies on this parameter does not distinguish/care about the difference so we simply
        // always pass 'aggregate'.
        ResourceYielderFactory::get(*pExpCtx->getOperationContext()->getService())
            .make(pExpCtx->getOperationContext(), "aggregate"_sd));
    _armParams = boost::none;
    // '_blockingResultsMerger' now owns the cursors.
    _ownCursors = false;
}

std::unique_ptr<RouterStageMerge> DocumentSourceMergeCursors::convertToRouterStage() {
    tassert(9535003, "Expected conversion to happen before execution", !_blockingResultsMerger);
    return std::make_unique<RouterStageMerge>(pExpCtx->getOperationContext(),
                                              pExpCtx->getMongoProcessInterface()->taskExecutor,
                                              std::move(*_armParams));
}

DocumentSource::GetNextResult DocumentSourceMergeCursors::doGetNext() {
    if (!_blockingResultsMerger) {
        populateMerger();
    }

    auto next = uassertStatusOK(_blockingResultsMerger->next(pExpCtx->getOperationContext()));
    _stats.dataBearingNodeMetrics.add(_blockingResultsMerger->takeMetrics());
    if (next.isEOF()) {
        return GetNextResult::makeEOF();
    }
    return Document::fromBsonWithMetaData(*next.getResult());
}

Value DocumentSourceMergeCursors::serialize(const SerializationOptions& opts) const {
    if (_blockingResultsMerger) {
        return Value(Document{
            {kStageName, _blockingResultsMerger->asyncResultsMergerParams().toBSON(opts)}});
    }
    tassert(9535004, "_armParams must be set", _armParams);
    return Value(Document{{kStageName, _armParams->toBSON(opts)}});
}

boost::intrusive_ptr<DocumentSource> DocumentSourceMergeCursors::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(17026,
            "$mergeCursors stage expected an object as argument",
            elem.type() == BSONType::Object);
    auto ownedObj = elem.embeddedObject().getOwned();
    auto armParams = AsyncResultsMergerParams::parse(
        IDLParserContext(kStageName,
                         auth::ValidatedTenancyScope::get(expCtx->getOperationContext()),
                         expCtx->getNamespaceString().tenantId(),
                         SerializationContext::stateDefault()),
        ownedObj);
    return new DocumentSourceMergeCursors(expCtx, std::move(armParams), std::move(ownedObj));
}

boost::intrusive_ptr<DocumentSourceMergeCursors> DocumentSourceMergeCursors::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, AsyncResultsMergerParams params) {
    return new DocumentSourceMergeCursors(expCtx, std::move(params));
}

void DocumentSourceMergeCursors::detachFromOperationContext() {
    if (_blockingResultsMerger) {
        _blockingResultsMerger->detachFromOperationContext();
    }
}

void DocumentSourceMergeCursors::reattachToOperationContext(OperationContext* opCtx) {
    if (_blockingResultsMerger) {
        _blockingResultsMerger->reattachToOperationContext(opCtx);
    }
}

void DocumentSourceMergeCursors::doDispose() {
    if (_blockingResultsMerger) {
        tassert(9535005, "_ownCursors must not be set", !_ownCursors);
        _blockingResultsMerger->kill(pExpCtx->getOperationContext());
    } else if (_ownCursors) {
        populateMerger();
        _blockingResultsMerger->kill(pExpCtx->getOperationContext());
    }
}

void DocumentSourceMergeCursors::recordRemoteCursorShardIds(
    const std::vector<RemoteCursor>& remoteCursors) {
    for (const auto& remoteCursor : remoteCursors) {
        tassert(5549103, "Encountered invalid shard ID", !remoteCursor.getShardId().empty());
        _shardsWithCursors.emplace(remoteCursor.getShardId().toString());
    }
}

boost::optional<NamespaceString> DocumentSourceMergeCursors::getAsyncResultMergerParamsNss_forTest()
    const {
    if (_armParams) {
        return _armParams->getNss();
    }
    return boost::none;
}

}  // namespace mongo
