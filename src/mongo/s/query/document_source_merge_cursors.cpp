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

#include "mongo/s/query/document_source_merge_cursors.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/query/cluster_query_result.h"
#include "mongo/s/resource_yielders.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(mergeCursors,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceMergeCursors::createFromBson,
                         AllowedWithApiStrict::kInternal);

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
    if (_armParams) {
        return _armParams->getRemotes().size();
    }
    return _blockingResultsMerger->getNumRemotes();
}

BSONObj DocumentSourceMergeCursors::getHighWaterMark() {
    if (!_blockingResultsMerger) {
        populateMerger();
    }
    return _blockingResultsMerger->getHighWaterMark();
}

bool DocumentSourceMergeCursors::remotesExhausted() const {
    if (_armParams) {
        // We haven't started iteration yet.
        return false;
    }
    return _blockingResultsMerger->remotesExhausted();
}

void DocumentSourceMergeCursors::populateMerger() {
    invariant(!_blockingResultsMerger);
    invariant(_armParams);

    _blockingResultsMerger.emplace(
        pExpCtx->opCtx,
        std::move(*_armParams),
        pExpCtx->mongoProcessInterface->taskExecutor,
        // Assumes this is only called from the 'aggregate' or 'getMore' commands.  The code which
        // relies on this parameter does not distinguish/care about the difference so we simply
        // always pass 'aggregate'.
        ResourceYielderFactory::get(*pExpCtx->opCtx->getService())
            .make(pExpCtx->opCtx, "aggregate"_sd));
    _armParams = boost::none;
    // '_blockingResultsMerger' now owns the cursors.
    _ownCursors = false;
}

std::unique_ptr<RouterStageMerge> DocumentSourceMergeCursors::convertToRouterStage() {
    invariant(!_blockingResultsMerger, "Expected conversion to happen before execution");
    return std::make_unique<RouterStageMerge>(
        pExpCtx->opCtx, pExpCtx->mongoProcessInterface->taskExecutor, std::move(*_armParams));
}

DocumentSource::GetNextResult DocumentSourceMergeCursors::doGetNext() {
    if (!_blockingResultsMerger) {
        populateMerger();
    }

    auto next = uassertStatusOK(_blockingResultsMerger->next(pExpCtx->opCtx));
    if (next.isEOF()) {
        return GetNextResult::makeEOF();
    }
    return Document::fromBsonWithMetaData(*next.getResult());
}

Value DocumentSourceMergeCursors::serialize(const SerializationOptions& opts) const {
    invariant(!_blockingResultsMerger);
    invariant(_armParams);
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
                         false /*apiStrict*/,
                         auth::ValidatedTenancyScope::get(expCtx->opCtx),
                         expCtx->ns.tenantId(),
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
        invariant(!_ownCursors);
        _blockingResultsMerger->kill(pExpCtx->opCtx);
    } else if (_ownCursors) {
        populateMerger();
        _blockingResultsMerger->kill(pExpCtx->opCtx);
    }
}


void DocumentSourceMergeCursors::recordRemoteCursorShardIds(
    const std::vector<RemoteCursor>& remoteCursors) {
    for (const auto& remoteCursor : remoteCursors) {
        tassert(5549103, "Encountered invalid shard ID", !remoteCursor.getShardId().empty());
        _shardsWithCursors.emplace(remoteCursor.getShardId().toString());
    }
}

}  // namespace mongo
