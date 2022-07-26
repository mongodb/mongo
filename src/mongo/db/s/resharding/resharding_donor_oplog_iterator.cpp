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

#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"

#include <tuple>
#include <utility>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/document_source_unwind.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {

using namespace fmt::literals;

namespace {

/**
 * Extracts the oplog id from the oplog.
 */
ReshardingDonorOplogId getId(const repl::OplogEntry& oplog) {
    return ReshardingDonorOplogId::parse(
        IDLParserContext("ReshardingDonorOplogIterator::getOplogId"),
        oplog.get_id()->getDocument().toBson());
}

}  // anonymous namespace

ReshardingDonorOplogIterator::ReshardingDonorOplogIterator(
    NamespaceString oplogBufferNss,
    ReshardingDonorOplogId resumeToken,
    resharding::OnInsertAwaitable* insertNotifier)
    : _oplogBufferNss(std::move(oplogBufferNss)),
      _resumeToken(std::move(resumeToken)),
      _insertNotifier(insertNotifier) {}

std::unique_ptr<Pipeline, PipelineDeleter> ReshardingDonorOplogIterator::makePipeline(
    OperationContext* opCtx, std::shared_ptr<MongoProcessInterface> mongoProcessInterface) {
    using Doc = Document;
    using Arr = std::vector<Value>;
    using V = Value;

    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[_oplogBufferNss.coll()] = {_oplogBufferNss, std::vector<BSONObj>{}};

    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    boost::none, /* explain */
                                                    false,       /* fromMongos */
                                                    false,       /* needsMerge */
                                                    false,       /* allowDiskUse */
                                                    false,       /* bypassDocumentValidation */
                                                    false,       /* isMapReduceCommand */
                                                    _oplogBufferNss,
                                                    boost::none, /* runtimeConstants */
                                                    nullptr,     /* collator */
                                                    std::move(mongoProcessInterface),
                                                    std::move(resolvedNamespaces),
                                                    boost::none /* collUUID */);

    Pipeline::SourceContainer stages;

    stages.emplace_back(
        DocumentSourceMatch::create(BSON("_id" << BSON("$gt" << _resumeToken.toBSON())), expCtx));

    stages.emplace_back(DocumentSourceSort::create(expCtx, BSON("_id" << 1)));

    return Pipeline::create(std::move(stages), std::move(expCtx));
}

std::vector<repl::OplogEntry> ReshardingDonorOplogIterator::_fillBatch(Pipeline& pipeline) {
    std::vector<repl::OplogEntry> batch;

    int numBytes = 0;
    do {
        auto doc = pipeline.getNext();
        if (!doc) {
            break;
        }


        auto obj = doc->toBson();
        auto& entry = batch.emplace_back(obj.getOwned());

        numBytes += obj.objsize();

        if (resharding::isFinalOplog(entry)) {
            // The ReshardingOplogFetcher should never insert documents after the reshardFinalOp
            // entry. We defensively check each oplog entry for being the reshardFinalOp and confirm
            // the pipeline has been exhausted.
            if (auto nextDoc = pipeline.getNext()) {
                tasserted(6077499,
                          fmt::format("Unexpectedly found entry after reshardFinalOp: {}",
                                      redact(nextDoc->toString())));
            }
        }
    } while (numBytes < resharding::gReshardingOplogBatchLimitBytes.load() &&
             batch.size() < std::size_t(resharding::gReshardingOplogBatchLimitOperations.load()));

    return batch;
}

ExecutorFuture<std::vector<repl::OplogEntry>> ReshardingDonorOplogIterator::getNextBatch(
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken cancelToken,
    CancelableOperationContextFactory factory) {
    if (_hasSeenFinalOplogEntry) {
        invariant(!_pipeline);
        return ExecutorFuture(std::move(executor), std::vector<repl::OplogEntry>{});
    }

    auto batch = [&] {
        auto opCtx = factory.makeOperationContext(&cc());
        ScopeGuard guard([&] { dispose(opCtx.get()); });

        // A primary which steps down may briefly continue running the ReshardingDonorOplogIterator
        // as a secondary. AutoGetCollectionForReadBase forbids reads on a secondary from using the
        // default RecoveryUnit::ReadSource of kNoTimestamp when the operation expects to conflict
        // with secondary oplog application. We opt out of acquiring the PBWM lock to avoid
        // triggering an fassert() when briefly running as a secondary. This is acceptable because
        // the PBWM lock isn't needed as a primary and any inconsistent reads as a secondary won't
        // have a real effect because the node won't be able to perform more writes.
        ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(
            opCtx->lockState());

        Timer fetchTimer;
        if (_pipeline) {
            _pipeline->reattachToOperationContext(opCtx.get());
        } else {
            auto pipeline = makePipeline(opCtx.get(), MongoProcessInterface::create(opCtx.get()));
            _pipeline = pipeline->getContext()
                            ->mongoProcessInterface->attachCursorSourceToPipelineForLocalRead(
                                pipeline.release());
            _pipeline.get_deleter().dismissDisposal();
        }

        auto batch = _fillBatch(*_pipeline);

        if (!batch.empty()) {
            const auto& lastEntryInBatch = batch.back();
            _resumeToken = getId(lastEntryInBatch);

            if (resharding::isFinalOplog(lastEntryInBatch)) {
                _hasSeenFinalOplogEntry = true;
                // Skip returning the final oplog entry because it is known to be a no-op.
                batch.pop_back();
            } else {
                _pipeline->detachFromOperationContext();
                guard.dismiss();
            }
        }

        return batch;
    }();

    if (batch.empty() && !_hasSeenFinalOplogEntry) {
        return ExecutorFuture(executor)
            .then([this, cancelToken] {
                return future_util::withCancellation(_insertNotifier->awaitInsert(_resumeToken),
                                                     cancelToken);
            })
            .then([this, cancelToken, executor, factory] {
                return getNextBatch(std::move(executor), cancelToken, factory);
            });
    }

    return ExecutorFuture(std::move(executor), std::move(batch));
}

void ReshardingDonorOplogIterator::dispose(OperationContext* opCtx) {
    if (_pipeline) {
        _pipeline->dispose(opCtx);
        _pipeline.reset();
    }
}

}  // namespace mongo
