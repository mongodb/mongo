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

#pragma once

#include "boost/smart_ptr/intrusive_ptr.hpp"
#include <cstdint>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/transaction_api.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

/**
 * FLE Result enum
 */
enum class FLEBatchResult {
    /**
     * FLE CRUD code decided input document requires FLE processing. Caller should not do any CRUD.
     */
    kProcessed,

    /**
     * FLE CRUD code decided it did not have to do any CRUD. For instance, it has no encrypted
     * fields that require further processing. Caller should process the request normally.
     */
    kNotProcessed
};

/**
 * Process a batch from mongos.
 */
FLEBatchResult processFLEBatch(OperationContext* opCtx,
                               const BatchedCommandRequest& request,
                               BatchWriteExecStats* stats,
                               BatchedCommandResponse* response,
                               boost::optional<OID> targetEpoch);


/**
 * Initialize the FLE CRUD subsystem on Mongod.
 */
void startFLECrud(ServiceContext* serviceContext);

/**
 * Stop the FLE CRUD subsystem on Mongod.
 */
void stopFLECrud();


/**
 * Process a replica set insert.
 */
FLEBatchResult processFLEInsert(OperationContext* opCtx,
                                const write_ops::InsertCommandRequest& insertRequest,
                                write_ops::InsertCommandReply* insertReply);

/**
 * Process a replica set delete.
 */
write_ops::DeleteCommandReply processFLEDelete(
    OperationContext* opCtx, const write_ops::DeleteCommandRequest& deleteRequest);

/**
 * Process a replica set update.
 */
write_ops::UpdateCommandReply processFLEUpdate(
    OperationContext* opCtx, const write_ops::UpdateCommandRequest& updateRequest);

/**
 * Process a findAndModify request from mongos
 */
FLEBatchResult processFLEFindAndModify(OperationContext* opCtx,
                                       const std::string& dbName,
                                       const BSONObj& cmdObj,
                                       BSONObjBuilder& result);

/**
 * Process a findAndModify request from a replica set.
 */
write_ops::FindAndModifyCommandReply processFLEFindAndModify(
    OperationContext* opCtx, const write_ops::FindAndModifyCommandRequest& findAndModifyRequest);

/**
 * Process a find command from mongos.
 */
void processFLEFindS(OperationContext* opCtx, FindCommandRequest* findCommand);

/**
 * Process a find command from a replica set.
 */
void processFLEFindD(OperationContext* opCtx, FindCommandRequest* findCommand);

/**
 * Helper function to determine if an IDL object with encryption information should be rewritten.
 */
template <typename T>
bool shouldDoFLERewrite(const std::unique_ptr<T>& cmd) {
    return gFeatureFlagFLE2.isEnabledAndIgnoreFCV() && cmd->getEncryptionInformation();
}

template <typename T>
bool shouldDoFLERewrite(const T& cmd) {
    return gFeatureFlagFLE2.isEnabledAndIgnoreFCV() && cmd.getEncryptionInformation();
}

/**
 * Abstraction layer for FLE
 */
class FLEQueryInterface {
public:
    virtual ~FLEQueryInterface();

    /**
     * Retrieve a single document by _id == BSONElement from nss.
     *
     * Returns an empty BSONObj if no document is found.
     * Expected to throw an error if it detects more then one documents.
     */
    virtual BSONObj getById(const NamespaceString& nss, BSONElement element) = 0;

    /**
     * Count the documents in the collection.
     *
     * Throws if the collection is not found.
     */
    virtual uint64_t countDocuments(const NamespaceString& nss) = 0;

    /**
     * Insert a document into the given collection.
     *
     * If translateDuplicateKey == true and the insert returns DuplicateKey, returns
     * FLEStateCollectionContention instead.
     */
    virtual StatusWith<write_ops::InsertCommandReply> insertDocument(
        const NamespaceString& nss, BSONObj obj, bool translateDuplicateKey) = 0;

    /**
     * Delete a single document with the given query.
     *
     * Returns the pre-image of the deleted document. If no documents were deleted, returns an empty
     * BSON object.
     */
    virtual std::pair<write_ops::DeleteCommandReply, BSONObj> deleteWithPreimage(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::DeleteCommandRequest& deleteRequest) = 0;

    /**
     * Update a single document with the given query and update operators.
     *
     * Returns the pre-image of the updated document. If no documents were updated, returns an empty
     * BSON object.
     */
    virtual std::pair<write_ops::UpdateCommandReply, BSONObj> updateWithPreimage(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::UpdateCommandRequest& updateRequest) = 0;

    /**
     * Do a single findAndModify request.
     *
     * TODO
     */
    virtual write_ops::FindAndModifyCommandReply findAndModify(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) = 0;
};
/**
 * Implementation of the FLE Query interface that exposes the DB operations needed for FLE 2
 * server-side work.
 */
class FLEQueryInterfaceImpl : public FLEQueryInterface {
public:
    FLEQueryInterfaceImpl(const txn_api::TransactionClient& txnClient) : _txnClient(txnClient) {}

    BSONObj getById(const NamespaceString& nss, BSONElement element) final;

    uint64_t countDocuments(const NamespaceString& nss) final;

    StatusWith<write_ops::InsertCommandReply> insertDocument(const NamespaceString& nss,
                                                             BSONObj obj,
                                                             bool translateDuplicateKey) final;

    std::pair<write_ops::DeleteCommandReply, BSONObj> deleteWithPreimage(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::DeleteCommandRequest& deleteRequest) final;

    std::pair<write_ops::UpdateCommandReply, BSONObj> updateWithPreimage(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::UpdateCommandRequest& updateRequest) final;

    write_ops::FindAndModifyCommandReply findAndModify(
        const NamespaceString& nss,
        const EncryptionInformation& ei,
        const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) final;

private:
    const txn_api::TransactionClient& _txnClient;
};

/**
 * Implementation of FLEStateCollectionReader for txn_api::TransactionClient
 *
 * Document count is cached since we only need it once per esc or ecc collection.
 */
class TxnCollectionReader : public FLEStateCollectionReader {
public:
    TxnCollectionReader(uint64_t count, FLEQueryInterface* queryImpl, const NamespaceString& nss)
        : _count(count), _queryImpl(queryImpl), _nss(nss) {}

    uint64_t getDocumentCount() const override {
        return _count;
    }

    BSONObj getById(PrfBlock block) const override {
        auto doc = BSON("v" << BSONBinData(block.data(), block.size(), BinDataGeneral));
        BSONElement element = doc.firstElement();
        return _queryImpl->getById(_nss, element);
    }

private:
    uint64_t _count;
    FLEQueryInterface* _queryImpl;
    NamespaceString _nss;
};

/**
 * Process a FLE insert with the query interface
 *
 * Used by unit tests.
 */
StatusWith<write_ops::InsertCommandReply> processInsert(
    FLEQueryInterface* queryImpl,
    const NamespaceString& edcNss,
    std::vector<EDCServerPayloadInfo>& serverPayload,
    const EncryptedFieldConfig& efc,
    BSONObj document);

/**
 * Process a FLE delete with the query interface
 *
 * Used by unit tests.
 */
write_ops::DeleteCommandReply processDelete(FLEQueryInterface* queryImpl,
                                            boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const write_ops::DeleteCommandRequest& deleteRequest);

/**
 * Process a FLE Update with the query interface
 *
 * Used by unit tests.
 */
write_ops::UpdateCommandReply processUpdate(FLEQueryInterface* queryImpl,
                                            boost::intrusive_ptr<ExpressionContext> expCtx,
                                            const write_ops::UpdateCommandRequest& updateRequest);

/**
 * Process a FLE Find And Modify with the query interface
 *
 * Used by unit tests.
 */
write_ops::FindAndModifyCommandReply processFindAndModify(
    FLEQueryInterface* queryImpl,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest);

/**
 * Callback function to get a TransactionWithRetries with the appropiate Executor
 */
using GetTxnCallback =
    std::function<std::shared_ptr<txn_api::TransactionWithRetries>(OperationContext*)>;

std::pair<FLEBatchResult, write_ops::InsertCommandReply> processInsert(
    OperationContext* opCtx,
    const write_ops::InsertCommandRequest& insertRequest,
    GetTxnCallback getTxns);

write_ops::DeleteCommandReply processDelete(OperationContext* opCtx,
                                            const write_ops::DeleteCommandRequest& deleteRequest,
                                            GetTxnCallback getTxns);

StatusWith<write_ops::FindAndModifyCommandReply> processFindAndModifyRequest(
    OperationContext* opCtx,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest,
    GetTxnCallback getTxns);


write_ops::UpdateCommandReply processUpdate(OperationContext* opCtx,
                                            const write_ops::UpdateCommandRequest& updateRequest,
                                            GetTxnCallback getTxns);
}  // namespace mongo
