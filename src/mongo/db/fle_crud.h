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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
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
 * Abstraction layer for FLE
 */
class FLEQueryInterface {
public:
    virtual ~FLEQueryInterface();

    /**
     * Retrieve a single document by _id == PrfBlock from nss.
     *
     * Returns an empty BSONObj if no document is found.
     * Expected to throw an error if it detects more then one documents.
     */
    virtual BSONObj getById(const NamespaceString& nss, PrfBlock block) = 0;

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
     * FLEStateCollectionContention instead
     */
    virtual void insertDocument(const NamespaceString& nss,
                                BSONObj obj,
                                bool translateDuplicateKey) = 0;

    /**
     * Delete a single document with the given query.
     *
     * Returns the pre-image of the deleted document.
     */
    virtual BSONObj deleteWithPreimage(const NamespaceString& nss, BSONObj query) = 0;
};

/**
 * Process a FLE insert with the query interface
 *
 * Used by unit tests.
 */
void processInsert(FLEQueryInterface* queryImpl,
                   const NamespaceString& edcNss,
                   std::vector<EDCServerPayloadInfo>& serverPayload,
                   const EncryptedFieldConfig& efc,
                   BSONObj document);

/**
 * Process a FLE delete with the query interface
 *
 * Used by unit tests.
 */
void processDelete(FLEQueryInterface* queryImpl,
                   const NamespaceString& edcNss,
                   const EncryptionInformation& ei,
                   BSONObj deleteQuery);
}  // namespace mongo
