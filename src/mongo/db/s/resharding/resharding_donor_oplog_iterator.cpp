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
#include "mongo/db/s/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

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

    stages.emplace_back(DocumentSourceReplaceRoot::createFromBson(
        BSON("$replaceWith" << BSON(kActualOpFieldName << "$$ROOT")).firstElement(), expCtx));

    for (const auto& [tsFieldName, opFieldName] :
         std::initializer_list<std::pair<std::string, StringData>>{
             {"$" + kActualOpFieldName + ".preImageOpTime.ts", kPreImageOpFieldName},
             {"$" + kActualOpFieldName + ".postImageOpTime.ts", kPostImageOpFieldName}}) {

        stages.emplace_back(DocumentSourceLookUp::createFromBson(
            Doc{{"$lookup",
                 Doc{{"from", _oplogBufferNss.coll()},
                     {"let",
                      Doc{{"preOrPostImageId",
                           Doc{{"clusterTime", tsFieldName}, {"ts", tsFieldName}}}}},
                     {"pipeline", Arr{V{Doc(fromjson("{$match: {$expr: {\
                        $eq: ['$_id', '$$preOrPostImageId']}}}"))}}},
                     {"as", opFieldName}}}}
                .toBson()
                .firstElement(),
            expCtx));

        stages.emplace_back(DocumentSourceUnwind::create(expCtx,
                                                         opFieldName.toString(),
                                                         true /* preserveNullAndEmptyArrays */,
                                                         boost::none /* includeArrayIndex */));
    }

    return Pipeline::create(std::move(stages), std::move(expCtx));
}

template <typename Callable>
auto ReshardingDonorOplogIterator::_withTemporaryOperationContext(Callable&& callable) {
    auto& client = cc();
    {
        stdx::lock_guard<Client> lk(client);
        invariant(client.canKillSystemOperationInStepdown(lk));
    }

    auto opCtx = client.makeOperationContext();
    opCtx->setAlwaysInterruptAtStepDownOrUp();

    return callable(opCtx.get());
}

std::vector<repl::OplogEntry> ReshardingDonorOplogIterator::_fillBatch(Pipeline& pipeline) {
    std::vector<repl::OplogEntry> batch;

    int numBytes = 0;
    do {
        auto doc = pipeline.getNext();
        if (!doc) {
            break;
        }

        Value actualOp, preImageOp, postImageOp;
        auto iter = doc->fieldIterator();
        while (iter.more()) {
            StringData field;
            Value value;
            std::tie(field, value) = iter.next();

            if (kActualOpFieldName == field) {
                actualOp = std::move(value);
            } else if (kPreImageOpFieldName == field) {
                preImageOp = std::move(value);
            } else if (kPostImageOpFieldName == field) {
                postImageOp = std::move(value);
            } else {
                uasserted(4990404,
                          str::stream() << "Unexpected top-level field from pipeline for iterating"
                                           " donor's oplog buffer: "
                                        << field);
            }
        }

        uassert(4990405, "Expected nested document for 'actualOp' field", actualOp.isObject());

        auto obj = actualOp.getDocument().toBson();
        auto& entry = batch.emplace_back(obj.getOwned());

        if (!preImageOp.missing()) {
            uassert(
                4990406, "Expected nested document for 'preImageOp' field", preImageOp.isObject());
            entry.setPreImageOp(preImageOp.getDocument().toBson());
        }

        if (!postImageOp.missing()) {
            uassert(4990407,
                    "Expected nested document for 'postImageOp' field",
                    postImageOp.isObject());
            entry.setPostImageOp(postImageOp.getDocument().toBson());
        }

        numBytes += obj.objsize();
    } while (numBytes < resharding::gReshardingBatchLimitBytes &&
             batch.size() < std::size_t(resharding::gReshardingBatchLimitOperations));

    return batch;
}

ExecutorFuture<std::vector<repl::OplogEntry>> ReshardingDonorOplogIterator::getNextBatch(
    std::shared_ptr<executor::TaskExecutor> executor) {
    if (_hasSeenFinalOplogEntry) {
        invariant(!_pipeline);
        return ExecutorFuture(std::move(executor), std::vector<repl::OplogEntry>{});
    }

    auto batch = _withTemporaryOperationContext([&](auto* opCtx) {
        if (_pipeline) {
            _pipeline->reattachToOperationContext(opCtx);
        } else {
            auto pipeline = makePipeline(opCtx, MongoProcessInterface::create(opCtx));
            _pipeline = pipeline->getContext()
                            ->mongoProcessInterface->attachCursorSourceToPipelineForLocalRead(
                                pipeline.release());
        }

        auto batch = _fillBatch(*_pipeline);

        if (batch.empty()) {
            _pipeline.reset();
        } else {
            const auto& lastEntryInBatch = batch.back();
            _resumeToken = getId(lastEntryInBatch);

            if (isFinalOplog(lastEntryInBatch)) {
                _hasSeenFinalOplogEntry = true;
                // Skip returning the final oplog entry because it is known to be a no-op.
                batch.pop_back();
                _pipeline.reset();
            } else {
                _pipeline->detachFromOperationContext();
            }
        }

        return batch;
    });

    if (batch.empty() && !_hasSeenFinalOplogEntry) {
        return ExecutorFuture(executor)
            .then([this] { return _insertNotifier->awaitInsert(_resumeToken); })
            .then([this, executor] { return getNextBatch(std::move(executor)); });
    }

    return ExecutorFuture(std::move(executor), std::move(batch));
}

}  // namespace mongo
