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


#include "mongo/platform/basic.h"

#include "mongo/db/keys_collection_client_direct.h"

#include <boost/optional.hpp>
#include <vector>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace {

const int kOnErrorNumRetries = 3;

bool isRetriableError(ErrorCodes::Error code, Shard::RetryPolicy options) {
    if (options == Shard::RetryPolicy::kNoRetry) {
        return false;
    }

    if (options == Shard::RetryPolicy::kIdempotent) {
        return code == ErrorCodes::WriteConcernFailed;
    } else {
        invariant(options == Shard::RetryPolicy::kNotIdempotent);
        return false;
    }
}

}  // namespace

KeysCollectionClientDirect::KeysCollectionClientDirect() : _rsLocalClient() {}

StatusWith<std::vector<KeysCollectionDocument>> KeysCollectionClientDirect::getNewInternalKeys(
    OperationContext* opCtx,
    StringData purpose,
    const LogicalTime& newerThanThis,
    bool useMajority) {
    return _getNewKeys<KeysCollectionDocument>(
        opCtx, NamespaceString::kKeysCollectionNamespace, purpose, newerThanThis, useMajority);
}

StatusWith<std::vector<ExternalKeysCollectionDocument>>
KeysCollectionClientDirect::getAllExternalKeys(OperationContext* opCtx, StringData purpose) {
    return _getNewKeys<ExternalKeysCollectionDocument>(
        opCtx,
        NamespaceString::kExternalKeysCollectionNamespace,
        purpose,
        LogicalTime(),
        // It is safe to read external keys with local read concern because they are only used to
        // validate incoming signatures, not to sign them. If a cached key is rolled back, it will
        // eventually be reaped from the cache.
        false /* useMajority */);
}

template <typename KeyDocumentType>
StatusWith<std::vector<KeyDocumentType>> KeysCollectionClientDirect::_getNewKeys(
    OperationContext* opCtx,
    const NamespaceString& nss,
    StringData purpose,
    const LogicalTime& newerThanThis,
    bool useMajority) {
    BSONObjBuilder queryBuilder;
    queryBuilder.append("purpose", purpose);
    queryBuilder.append("expiresAt", BSON("$gt" << newerThanThis.asTimestamp()));

    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto readConcern = storageEngine->supportsReadConcernMajority() && useMajority
        ? repl::ReadConcernLevel::kMajorityReadConcern
        : repl::ReadConcernLevel::kLocalReadConcern;

    auto findStatus = _query(opCtx,
                             ReadPreferenceSetting(ReadPreference::Nearest, TagSet{}),
                             readConcern,
                             nss,
                             queryBuilder.obj(),
                             BSON("expiresAt" << 1),
                             boost::none);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& keyDocs = findStatus.getValue().docs;
    std::vector<KeyDocumentType> keys;
    for (auto&& keyDoc : keyDocs) {
        KeyDocumentType key;
        try {
            key = KeyDocumentType::parse(IDLParserContext("keyDoc"), keyDoc);
        } catch (...) {
            return exceptionToStatus();
        }
        keys.push_back(std::move(key));
    }

    return keys;
}

StatusWith<Shard::QueryResponse> KeysCollectionClientDirect::_query(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const repl::ReadConcernLevel& readConcernLevel,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {

    for (int retry = 1; retry <= kOnErrorNumRetries; retry++) {
        auto result =
            _rsLocalClient.queryOnce(opCtx, readPref, readConcernLevel, nss, query, sort, limit);

        if (retry < kOnErrorNumRetries &&
            isRetriableError(result.getStatus().code(), Shard::RetryPolicy::kIdempotent)) {
            continue;
        }

        return result;
    }
    MONGO_UNREACHABLE;
}

Status KeysCollectionClientDirect::_insert(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const BSONObj& doc,
                                           const WriteConcernOptions& writeConcern) {
    BatchedCommandRequest request([&] {
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({doc});
        return insertOp;
    }());
    request.setWriteConcern(writeConcern.toBSON());
    const BSONObj cmdObj = request.toBSON();

    for (int retry = 1; retry <= kOnErrorNumRetries; ++retry) {
        // Note: write commands can only be issued against a primary.
        auto swResponse = _rsLocalClient.runCommandOnce(opCtx, nss.db().toString(), cmdObj);

        BatchedCommandResponse batchResponse;
        auto writeStatus =
            Shard::CommandResponse::processBatchWriteResponse(swResponse, &batchResponse);
        if (retry < kOnErrorNumRetries &&
            isRetriableError(writeStatus.code(), Shard::RetryPolicy::kIdempotent)) {
            LOGV2_DEBUG(20704,
                        2,
                        "Batch write command to {nss_db}failed with retriable error and will be "
                        "retried{causedBy_writeStatus}",
                        "nss_db"_attr = nss.db(),
                        "causedBy_writeStatus"_attr = causedBy(redact(writeStatus)));
            continue;
        }

        return batchResponse.toStatus();
    }
    MONGO_UNREACHABLE;
}

Status KeysCollectionClientDirect::insertNewKey(OperationContext* opCtx, const BSONObj& doc) {
    return _insert(opCtx,
                   NamespaceString::kKeysCollectionNamespace,
                   doc,
                   ShardingCatalogClient::kMajorityWriteConcern);
}

}  // namespace mongo
