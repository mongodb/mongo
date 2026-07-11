// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/keys_collection_client_direct.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"

#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace {

const int kOnErrorNumRetries = 3;

bool isRetriableError(ErrorCodes::Error code, Shard::RetryPolicy options) {
    if (options == Shard::RetryPolicy::kNoRetry) {
        return false;
    }

    if (options == Shard::RetryPolicy::kIdempotent) {
        return code == ErrorCodes::WriteConcernTimeout;
    } else {
        invariant(options == Shard::RetryPolicy::kNotIdempotent);
        return false;
    }
}

}  // namespace

KeysCollectionClientDirect::KeysCollectionClientDirect(bool mustUseLocalReads)
    : _rsLocalClient(), _mustUseLocalReads(mustUseLocalReads) {}

StatusWith<std::vector<KeysCollectionDocument>> KeysCollectionClientDirect::getNewInternalKeys(
    OperationContext* opCtx,
    std::string_view purpose,
    const LogicalTime& newerThanThis,
    bool tryUseMajority) {

    return _getNewKeys<KeysCollectionDocument>(
        opCtx, NamespaceString::kKeysCollectionNamespace, purpose, newerThanThis, tryUseMajority);
}

StatusWith<std::vector<ExternalKeysCollectionDocument>>
KeysCollectionClientDirect::getAllExternalKeys(OperationContext* opCtx, std::string_view purpose) {
    return _getNewKeys<ExternalKeysCollectionDocument>(
        opCtx,
        NamespaceString::kExternalKeysCollectionNamespace,
        purpose,
        LogicalTime(),
        // It is safe to read external keys with local read concern because they are only used to
        // validate incoming signatures, not to sign them. If a cached key is rolled back, it will
        // eventually be reaped from the cache.
        false /* tryUseMajority */);
}

template <typename KeyDocumentType>
StatusWith<std::vector<KeyDocumentType>> KeysCollectionClientDirect::_getNewKeys(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::string_view purpose,
    const LogicalTime& newerThanThis,
    bool tryUseMajority) {
    BSONObjBuilder queryBuilder;
    queryBuilder.append("purpose", purpose);
    queryBuilder.append("expiresAt", BSON("$gt" << newerThanThis.asTimestamp()));

    // Use majority read concern if the caller wants that and the client supports it. Otherwise fall
    // back to local read concern.
    const auto& readConcern = (tryUseMajority && !_mustUseLocalReads)
        ? repl::ReadConcernArgs::kMajority
        : repl::ReadConcernArgs::kLocal;

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
            key = KeyDocumentType::parse(keyDoc, IDLParserContext("keyDoc"));
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
    const repl::ReadConcernArgs& readConcern,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {

    for (int retry = 1; retry <= kOnErrorNumRetries; retry++) {
        auto result =
            _rsLocalClient.queryOnce(opCtx, readPref, readConcern, nss, query, sort, limit);

        if (retry < kOnErrorNumRetries &&
            isRetriableError(result.getStatus().code(), Shard::RetryPolicy::kIdempotent)) {
            continue;
        }

        return result;
    }
    MONGO_UNREACHABLE;
}

Status KeysCollectionClientDirect::_insert(OperationContext* opCtx,
                                           const BSONObj& doc,
                                           const WriteConcernOptions& writeConcern) {
    // TODO SERVER-88742 Just use write_ops::InsertCommandRequest
    BatchedCommandRequest batchRequest([&] {
        write_ops::InsertCommandRequest insertOp(NamespaceString::kKeysCollectionNamespace);
        insertOp.setDocuments({doc});
        return insertOp;
    }());

    // A request dispatched through a local client is served within the same thread that submits it
    // (so that the opCtx needs to be used as the vehicle to pass the WC to the ServiceEntryPoint).
    const auto originalWC = opCtx->getWriteConcern();
    ScopeGuard resetWCGuard([&] { opCtx->setWriteConcern(originalWC); });
    opCtx->setWriteConcern(writeConcern);

    const BSONObj cmdObj = [&] {
        BSONObjBuilder cmdObjBuilder;
        batchRequest.serialize(&cmdObjBuilder);
        return cmdObjBuilder.obj();
    }();

    for (int retry = 1; retry <= kOnErrorNumRetries; ++retry) {
        // Note: write commands can only be issued against a primary.
        auto swResponse = _rsLocalClient.runCommandOnce(
            opCtx, NamespaceString::kKeysCollectionNamespace.dbName(), cmdObj);

        BatchedCommandResponse batchResponse;
        auto writeStatus =
            Shard::CommandResponse::processBatchWriteResponse(swResponse, &batchResponse);
        if (retry < kOnErrorNumRetries &&
            isRetriableError(writeStatus.code(), Shard::RetryPolicy::kIdempotent)) {
            LOGV2_DEBUG(20704,
                        2,
                        "Batch write command to {nss_db}failed with retriable error and will be "
                        "retried{causedBy_writeStatus}",
                        "nss_db"_attr = NamespaceString::kKeysCollectionNamespace.db(omitTenant),
                        "causedBy_writeStatus"_attr = causedBy(redact(writeStatus)));
            continue;
        }

        return batchResponse.toStatus();
    }
    MONGO_UNREACHABLE;
}

Status KeysCollectionClientDirect::insertNewKey(OperationContext* opCtx, const BSONObj& doc) {
    return _insert(opCtx, doc, defaultMajorityWriteConcernDoNotUse());
}

}  // namespace mongo
