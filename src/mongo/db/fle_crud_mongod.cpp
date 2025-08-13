/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/crypto/fle_stats_gen.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"
#include "mongo/db/query/fle/server_rewrite.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/decorable.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

std::shared_ptr<executor::ThreadPoolTaskExecutor> _fleCrudExecutor;

ThreadPool::Options getThreadPoolOptions() {
    ThreadPool::Options tpOptions;
    tpOptions.poolName = "FLECrud";
    tpOptions.maxThreads = ThreadPool::Options::kUnlimited;

    // SEPTransactionClient::runCommand manages the client itself so do not create one via
    // onCreateThread
    return tpOptions;
}

void setMongosFieldsInReply(OperationContext* opCtx, write_ops::WriteCommandReplyBase* replyBase) {
    // Update the OpTime for the reply to current OpTime
    //
    // The OpTime in the reply reflects the OpTime of when the request was run, not when it was
    // committed. The Transaction API propagates the OpTime from the commit transaction onto the
    // current thread so grab it from TLS and change the OpTime on the reply.
    //
    auto* replCoord = repl::ReplicationCoordinator::get(opCtx->getServiceContext());
    if (replCoord->getSettings().isReplSet()) {
        replyBase->setOpTime(repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp());
        replyBase->setElectionId(replCoord->getElectionId());
    }
}


class FLEMongoDResourceYielder : public ResourceYielder {
public:
    void yield(OperationContext* opCtx) override {
        // We're about to block. Check back in the session so that it's available to other
        // threads. Note that we may block on a request to _ourselves_, meaning that we may have to
        // wait for another thread which will use the same session. This step is necessary
        // to prevent deadlocks.

        Session* const session = OperationContextSession::get(opCtx);
        if (session) {
            if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
                txnParticipant.stashTransactionResources(opCtx);
            }

            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            mongoDSessionCatalog->checkInUnscopedSession(
                opCtx, OperationContextSession::CheckInReason::kYield);
        }
        _yielded = (session != nullptr);
    }

    void unyield(OperationContext* opCtx) override {
        if (_yielded) {
            // This may block on a sub-operation on this node finishing. It's possible that while
            // blocked on the network layer, another shard could have responded, theoretically
            // unblocking this thread of execution. However, we must wait until the child operation
            // on this shard finishes so we can get the session back. This may limit the throughput
            // of the operation, but it's correct.
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
            mongoDSessionCatalog->checkOutUnscopedSession(opCtx);

            if (auto txnParticipant = TransactionParticipant::get(opCtx)) {
                // Assumes this is only called from the 'aggregate' or 'getMore' commands.  The code
                // which relies on this parameter does not distinguish/care about the difference so
                // we simply always pass 'aggregate'.
                //
                // Catch NoSuchTransaction which happens when the transaction is aborted by an
                // unrelated error If this error is not caught, then a user error like DuplicateKey
                // gets ignored for NoSuchTransaction
                try {
                    txnParticipant.unstashTransactionResources(opCtx, "aggregate");
                } catch (ExceptionFor<ErrorCodes::NoSuchTransaction>&) {
                }
            }
        }
    }

private:
    bool _yielded = false;
};

void toBinData(StringData field, PrfBlock block, BSONObjBuilder* builder) {
    builder->appendBinData(field, block.size(), BinDataType::BinDataGeneral, block.data());
}


/**
 * If the collection is missing (which should not happen in practice), we create a mock reader that
 * just returns nothing rather then special case the algorithm for a missing collection.
 */
class MissingCollectionReader : public FLEStateCollectionReader {
public:
    uint64_t getDocumentCount() const override {
        return 0;
    }

    BSONObj getById(PrfBlock block) const override {

        return BSONObj();
    }

    ECStats getStats() const override {
        return ECStats();
    }
};


/**
 * Lookup a ESC document by _id by clustered index id.
 *
 * Clustered indexes mean we only have to hit one table in WT so this will be very quick.
 */
class StorageEngineClusteredCollectionReader : public FLEStateCollectionReader {
public:
    StorageEngineClusteredCollectionReader(OperationContext* opCtx,
                                           uint64_t count,
                                           const NamespaceStringOrUUID& nsOrUUID,
                                           SeekableRecordCursor* cursor)
        : _opCtx(opCtx), _count(count), _nssOrUUID(nsOrUUID), _cursor(cursor) {}

    uint64_t getDocumentCount() const override {
        return _count;
    }

    void incrementRead() const {
        _stats.setRead(_stats.getRead() + 1);
    }

    ECStats getStats() const override {
        return _stats;
    }

    BSONObj getById(PrfBlock block) const override {
        auto record = getRecordById(block);

        if (record.has_value()) {
            return record->data.releaseToBson();
        }

        return BSONObj();
    }

    bool existsById(PrfBlock block) const override {
        return getRecordById(block).has_value();
    }

private:
    boost::optional<Record> getRecordById(PrfBlock block) const {
        // Check for interruption so we can be killed
        _opCtx->checkForInterrupt();

        key_string::Builder builder(key_string::Version::kLatestVersion);
        builder.appendBinData(BSONBinData(block.data(), block.size(), BinDataType::BinDataGeneral));
        auto recordId = RecordId(builder.getView());

        incrementRead();
        return _cursor->seekExact(recordId);
    }

private:
    OperationContext* _opCtx;
    const uint64_t _count;
    const NamespaceStringOrUUID& _nssOrUUID;
    SeekableRecordCursor* _cursor;
    mutable ECStats _stats;
};

/**
 * Lookup a ESC document by _id in non-clustered collection.
 *
 * We have to lookup first by _id in the _id index and then get the document from the base
 * collection via its record id.
 */
class StorageEngineIndexCollectionReader : public FLEStateCollectionReader {
public:
    StorageEngineIndexCollectionReader(OperationContext* opCtx,
                                       uint64_t count,
                                       const NamespaceStringOrUUID& nsOrUUID,
                                       SeekableRecordCursor* cursor,
                                       SortedDataInterface* sdi,
                                       SortedDataInterface::Cursor* indexCursor)
        : _opCtx(opCtx),
          _count(count),
          _nssOrUUID(nsOrUUID),
          _sdi(sdi),
          _indexCursor(indexCursor),
          _cursor(cursor) {}

    uint64_t getDocumentCount() const override {
        return _count;
    }

    void incrementRead() const {
        _stats.setRead(_stats.getRead() + 1);
    }

    ECStats getStats() const override {
        return _stats;
    }

    BSONObj getById(PrfBlock block) const override {
        auto record = getRecordById(block);

        if (record.has_value()) {
            return record->data.releaseToBson();
        }

        return BSONObj();
    }

    bool existsById(PrfBlock block) const override {
        return getRecordById(block).has_value();
    }

private:
    boost::optional<Record> getRecordById(PrfBlock block) const {
        // Check for interruption so we can be killed
        _opCtx->checkForInterrupt();

        key_string::Builder kb(_sdi->getKeyStringVersion(),
                               _sdi->getOrdering(),
                               key_string::Discriminator::kInclusive);

        kb.appendBinData(BSONBinData(block.data(), block.size(), BinDataGeneral));

        incrementRead();

        auto& ru = *shard_role_details::getRecoveryUnit(_opCtx);
        auto loc = _indexCursor->seekExact(ru, kb.finishAndGetBuffer());
        if (!loc) {
            return boost::none;
        }

        // Get the document from the base collection
        return _cursor->seekExact(loc.get());
    }

private:
    OperationContext* _opCtx;
    const uint64_t _count;
    const NamespaceStringOrUUID& _nssOrUUID;
    SortedDataInterface* _sdi;
    SortedDataInterface::Cursor* _indexCursor;
    SeekableRecordCursor* _cursor;
    mutable ECStats _stats;
};
}  // namespace

std::shared_ptr<txn_api::SyncTransactionWithRetries> getTransactionWithRetriesForMongoD(
    OperationContext* opCtx) {

    auto fleInlineCrudExecutor = std::make_shared<executor::InlineExecutor>();

    return std::make_shared<txn_api::SyncTransactionWithRetries>(
        opCtx,
        _fleCrudExecutor,
        std::make_unique<FLEMongoDResourceYielder>(),
        fleInlineCrudExecutor);
}

void startFLECrud(ServiceContext* serviceContext) {
    // FLE crud is only supported on replica sets so no reason to start thread pool on standalones
    if (!repl::ReplicationCoordinator::get(serviceContext)->getSettings().isReplSet()) {
        return;
    }

    _fleCrudExecutor = executor::ThreadPoolTaskExecutor::create(
        std::make_unique<ThreadPool>(getThreadPoolOptions()),
        executor::makeNetworkInterface("FLECrudNetwork"));

    _fleCrudExecutor->startup();
}

void stopFLECrud() {
    // Check if it was started
    if (_fleCrudExecutor.get() != nullptr) {
        _fleCrudExecutor->shutdown();
        _fleCrudExecutor->join();
    }
}

FLEBatchResult processFLEInsert(OperationContext* opCtx,
                                const write_ops::InsertCommandRequest& insertRequest,
                                write_ops::InsertCommandReply* insertReply) {

    uassert(
        6371602,
        "Encrypted index operations are only supported on replica sets",
        repl::ReplicationCoordinator::get(opCtx->getServiceContext())->getSettings().isReplSet());

    auto [batchResult, insertReplyReturn] =
        processInsert(opCtx, insertRequest, &getTransactionWithRetriesForMongoD);

    if (batchResult == FLEBatchResult::kNotProcessed) {
        return FLEBatchResult::kNotProcessed;
    }

    *insertReply = insertReplyReturn;

    setMongosFieldsInReply(opCtx, &insertReply->getWriteCommandReplyBase());

    return FLEBatchResult::kProcessed;
}

write_ops::DeleteCommandReply processFLEDelete(
    OperationContext* opCtx, const write_ops::DeleteCommandRequest& deleteRequest) {

    uassert(
        6371701,
        "Encrypted index operations are only supported on replica sets",
        repl::ReplicationCoordinator::get(opCtx->getServiceContext())->getSettings().isReplSet());

    auto deleteReply = processDelete(opCtx, deleteRequest, &getTransactionWithRetriesForMongoD);

    setMongosFieldsInReply(opCtx, &deleteReply.getWriteCommandReplyBase());

    return deleteReply;
}

StatusWith<std::pair<write_ops::FindAndModifyCommandReply, OpMsgRequest>>
processFLEFindAndModifyHelper(OperationContext* opCtx,
                              const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) {

    uassert(
        6371800,
        "Encrypted index operations are only supported on replica sets",
        repl::ReplicationCoordinator::get(opCtx->getServiceContext())->getSettings().isReplSet());

    // TODO: SERVER-103506 remove this once TypedCommands can handle write concern errors better
    auto onErrorWithWCE = [&opCtx](const WriteConcernErrorDetail& wce) {
        // When a FLE2 findAndModify in mongod encounters a write error, the internal transaction
        // that tried to perform the encrypted writes may abort with a write concern error.
        // In such cases, the internal write concern error is lost because only the write error
        // gets thrown up to the command dispatch layer. Moreover, since the internal transaction's
        // writes were conducted under a different Client, the command's opCtx's Client's lastOpTime
        // never gets advanced, and so the command dispatch skips waiting for write concern.
        // Here, we set the opCtx's Client's lastOpTime to the system lastOpTime to indicate the
        // need to wait for write concern again at the command dispatch layer.
        if (!opCtx->inMultiDocumentTransaction()) {
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        }
    };

    auto reply = processFindAndModifyRequest<write_ops::FindAndModifyCommandReply>(
        opCtx,
        findAndModifyRequest,
        &getTransactionWithRetriesForMongoD,
        processFindAndModify,
        onErrorWithWCE);

    return reply;
}

write_ops::FindAndModifyCommandReply processFLEFindAndModify(
    OperationContext* opCtx, const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) {
    return uassertStatusOK(processFLEFindAndModifyHelper(opCtx, findAndModifyRequest)).first;
}

write_ops::UpdateCommandReply processFLEUpdate(
    OperationContext* opCtx, const write_ops::UpdateCommandRequest& updateRequest) {

    uassert(
        6371905,
        "Encrypted index operations are only supported on replica sets",
        repl::ReplicationCoordinator::get(opCtx->getServiceContext())->getSettings().isReplSet());

    auto updateReply = processUpdate(opCtx, updateRequest, &getTransactionWithRetriesForMongoD);

    setMongosFieldsInReply(opCtx, &updateReply.getWriteCommandReplyBase());

    return updateReply;
}

void processFLEFindD(OperationContext* opCtx,
                     const NamespaceString& nss,
                     FindCommandRequest* findCommand) {
    fle::processFindCommand(opCtx, nss, findCommand, &getTransactionWithRetriesForMongoD);
}

void processFLECountD(OperationContext* opCtx,
                      const NamespaceString& nss,
                      CountCommandRequest& countCommand) {
    fle::processCountCommand(opCtx, nss, &countCommand, &getTransactionWithRetriesForMongoD);
}

std::unique_ptr<Pipeline> processFLEPipelineD(OperationContext* opCtx,
                                              NamespaceString nss,
                                              const EncryptionInformation& encryptInfo,
                                              std::unique_ptr<Pipeline> toRewrite) {
    return fle::processPipeline(
        opCtx, nss, encryptInfo, std::move(toRewrite), &getTransactionWithRetriesForMongoD);
}

BSONObj processFLEWriteExplainD(OperationContext* opCtx,
                                const BSONObj& collation,
                                const NamespaceString& nss,
                                const EncryptionInformation& info,
                                const boost::optional<LegacyRuntimeConstants>& runtimeConstants,
                                const boost::optional<BSONObj>& letParameters,
                                const BSONObj& query) {

    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .collator(fle::collatorFromBSON(opCtx, collation))
                      .ns(nss)
                      .runtimeConstants(runtimeConstants)
                      .letParameters(letParameters)
                      .build();
    return fle::rewriteQuery(opCtx,
                             expCtx,
                             nss,
                             info,
                             query,
                             &getTransactionWithRetriesForMongoD,
                             fle::EncryptedCollScanModeAllowed::kAllow);
}

std::pair<write_ops::FindAndModifyCommandRequest, OpMsgRequest>
processFLEFindAndModifyExplainMongod(OperationContext* opCtx,
                                     const write_ops::FindAndModifyCommandRequest& request) {
    tassert(6513401,
            "Missing encryptionInformation for findAndModify",
            request.getEncryptionInformation().has_value());

    return uassertStatusOK(processFindAndModifyRequest<write_ops::FindAndModifyCommandRequest>(
        opCtx, request, &getTransactionWithRetriesForMongoD, processFindAndModifyExplain));
}

std::vector<std::vector<FLEEdgeCountInfo>> getTagsFromStorage(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& nsOrUUID,
    const std::vector<std::vector<FLEEdgePrfBlock>>& escDerivedFromDataTokens,
    FLETagQueryInterface::TagQueryType type) {

    auto opStr = "getTagsFromStorage"_sd;
    return writeConflictRetry(
        opCtx, opStr, nsOrUUID, [&]() -> std::vector<std::vector<FLEEdgeCountInfo>> {
            AutoGetCollectionForReadMaybeLockFree autoColl(opCtx, nsOrUUID);

            const auto& collection = autoColl.getCollection();

            // If there is no collection, run through the algorithm with a special reader that only
            // returns empty documents. This simplifies the implementation of other readers.
            if (!collection) {
                MissingCollectionReader reader;
                return ESCCollection::getTags(reader, escDerivedFromDataTokens, type);
            }

            // numRecords is signed so guard against negative numbers
            auto docCountSigned = collection->numRecords(opCtx);
            uint64_t docCount = docCountSigned < 0 ? 0 : static_cast<uint64_t>(docCountSigned);

            std::unique_ptr<SeekableRecordCursor> cursor = collection->getCursor(opCtx, true);

            // If clustered collection, we have simpler searches
            if (collection->isClustered() &&
                collection->getClusteredInfo()
                        ->getIndexSpec()
                        .getKey()
                        .firstElement()
                        .fieldNameStringData() == "_id"_sd) {

                StorageEngineClusteredCollectionReader reader(
                    opCtx, docCount, nsOrUUID, cursor.get());

                return ESCCollection::getTags(reader, escDerivedFromDataTokens, type);
            }

            // Non-clustered case, we need to look a index entry in _id index and then the
            // collection
            auto indexCatalog = collection->getIndexCatalog();

            const IndexDescriptor* indexDescriptor = indexCatalog->findIndexByName(
                opCtx, IndexConstants::kIdIndexName, IndexCatalog::InclusionPolicy::kReady);
            if (!indexDescriptor) {
                uasserted(ErrorCodes::IndexNotFound,
                          str::stream() << "Index not found, ns:" << toStringForLogging(nsOrUUID)
                                        << ", index: " << IndexConstants::kIdIndexName);
            }

            if (indexDescriptor->isPartial()) {
                uasserted(ErrorCodes::IndexOptionsConflict,
                          str::stream() << "Partial index is not allowed for this operation, ns:"
                                        << toStringForLogging(nsOrUUID)
                                        << ", index: " << IndexConstants::kIdIndexName);
            }

            auto indexCatalogEntry = indexDescriptor->getEntry()->shared_from_this();

            auto sdi = indexCatalogEntry->accessMethod()->asSortedData();
            auto indexCursor =
                sdi->newCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx), true);

            StorageEngineIndexCollectionReader reader(opCtx,
                                                      docCount,
                                                      nsOrUUID,
                                                      cursor.get(),
                                                      sdi->getSortedDataInterface(),
                                                      indexCursor.get());

            return ESCCollection::getTags(reader, escDerivedFromDataTokens, type);
        });
}

}  // namespace mongo
