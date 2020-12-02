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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"

#include <fmt/format.h>

#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using namespace fmt::literals;

namespace {

/**
 * Extracts the oplog id from the oplog.
 */
ReshardingDonorOplogId getId(const repl::OplogEntry& oplog) {
    return ReshardingDonorOplogId::parse(
        IDLParserErrorContext("ReshardingDonorOplogIterator::getOplogId"),
        oplog.get_id()->getDocument().toBson());
}

}  // anonymous namespace

ReshardingDonorOplogIterator::ReshardingDonorOplogIterator(
    NamespaceString donorOplogBufferNs,
    boost::optional<ReshardingDonorOplogId> resumeToken,
    resharding::OnInsertAwaitable* insertNotifier)
    : _oplogBufferNs(std::move(donorOplogBufferNs)),
      _resumeToken(std::move(resumeToken)),
      _insertNotifier(insertNotifier) {}

Future<boost::optional<repl::OplogEntry>> ReshardingDonorOplogIterator::getNext(
    OperationContext* opCtx) {
    boost::optional<repl::OplogEntry> oplogToReturn;

    if (_hasSeenFinalOplogEntry) {
        return Future<boost::optional<repl::OplogEntry>>::makeReady(oplogToReturn);
    }

    if (!_pipeline) {
        auto expCtx = _makeExpressionContext(opCtx);
        _pipeline = createAggForReshardingOplogBuffer(std::move(expCtx), _resumeToken, true);
        _pipeline->detachFromOperationContext();
    }

    _pipeline->reattachToOperationContext(opCtx);
    ON_BLOCK_EXIT([this] {
        if (_pipeline) {
            _pipeline->detachFromOperationContext();
        }
    });

    auto next = _pipeline->getNext();

    if (!next) {
        _pipeline.reset();
        return _waitForNewOplog().then([this, opCtx] { return getNext(opCtx); });
    }

    auto nextOplog = uassertStatusOK(repl::OplogEntry::parse(next->toBson()));
    if (isFinalOplog(nextOplog)) {
        _hasSeenFinalOplogEntry = true;
        _pipeline.reset();
        return Future<boost::optional<repl::OplogEntry>>::makeReady(oplogToReturn);
    }

    _resumeToken = getId(nextOplog);

    oplogToReturn = std::move(nextOplog);
    return Future<boost::optional<repl::OplogEntry>>::makeReady(std::move(oplogToReturn));
}

bool ReshardingDonorOplogIterator::hasMore() const {
    return !_hasSeenFinalOplogEntry;
}

Future<void> ReshardingDonorOplogIterator::_waitForNewOplog() {
    return _insertNotifier->awaitInsert(*_resumeToken);
}

boost::intrusive_ptr<ExpressionContext> ReshardingDonorOplogIterator::_makeExpressionContext(
    OperationContext* opCtx) {
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces.emplace(_oplogBufferNs.coll(),
                               ExpressionContext::ResolvedNamespace{_oplogBufferNs, {}});

    return make_intrusive<ExpressionContext>(opCtx,
                                             boost::none /* explain */,
                                             false /* fromMongos */,
                                             false /* needsMerge */,
                                             false /* allowDiskUse */,
                                             false /* bypassDocumentValidation */,
                                             false /* isMapReduceCommand */,
                                             _oplogBufferNs,
                                             boost::none /* runtimeConstants */,
                                             nullptr /* collator */,
                                             MongoProcessInterface::create(opCtx),
                                             std::move(resolvedNamespaces),
                                             boost::none /* collUUID */);
}

}  // namespace mongo
