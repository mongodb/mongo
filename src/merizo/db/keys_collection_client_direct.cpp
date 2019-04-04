/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kReplication

#include "merizo/platform/basic.h"

#include "merizo/db/keys_collection_client_direct.h"

#include <boost/optional.hpp>
#include <vector>

#include "merizo/bson/bsonobjbuilder.h"
#include "merizo/bson/util/bson_extract.h"
#include "merizo/client/read_preference.h"
#include "merizo/db/dbdirectclient.h"
#include "merizo/db/keys_collection_document.h"
#include "merizo/db/logical_clock.h"
#include "merizo/db/logical_time.h"
#include "merizo/db/operation_context.h"
#include "merizo/db/service_context.h"
#include "merizo/rpc/get_status_from_command_result.h"
#include "merizo/s/catalog/sharding_catalog_client.h"
#include "merizo/s/write_ops/batched_command_request.h"
#include "merizo/s/write_ops/batched_command_response.h"
#include "merizo/util/log.h"

namespace merizo {
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

StatusWith<std::vector<KeysCollectionDocument>> KeysCollectionClientDirect::getNewKeys(
    OperationContext* opCtx, StringData purpose, const LogicalTime& newerThanThis) {


    BSONObjBuilder queryBuilder;
    queryBuilder.append("purpose", purpose);
    queryBuilder.append("expiresAt", BSON("$gt" << newerThanThis.asTimestamp()));

    auto findStatus = _query(opCtx,
                             ReadPreferenceSetting(ReadPreference::Nearest, TagSet{}),
                             repl::ReadConcernLevel::kLocalReadConcern,
                             KeysCollectionDocument::ConfigNS,
                             queryBuilder.obj(),
                             BSON("expiresAt" << 1),
                             boost::none);

    if (!findStatus.isOK()) {
        return findStatus.getStatus();
    }

    const auto& keyDocs = findStatus.getValue().docs;
    std::vector<KeysCollectionDocument> keys;
    for (auto&& keyDoc : keyDocs) {
        auto parseStatus = KeysCollectionDocument::fromBSON(keyDoc);
        if (!parseStatus.isOK()) {
            return parseStatus.getStatus();
        }

        keys.push_back(std::move(parseStatus.getValue()));
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
    MERIZO_UNREACHABLE;
}

Status KeysCollectionClientDirect::_insert(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const BSONObj& doc,
                                           const WriteConcernOptions& writeConcern) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
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
            LOG(2) << "Batch write command to " << nss.db()
                   << "failed with retriable error and will be retried"
                   << causedBy(redact(writeStatus));
            continue;
        }

        return batchResponse.toStatus();
    }
    MERIZO_UNREACHABLE;
}

Status KeysCollectionClientDirect::insertNewKey(OperationContext* opCtx, const BSONObj& doc) {
    return _insert(
        opCtx, KeysCollectionDocument::ConfigNS, doc, ShardingCatalogClient::kMajorityWriteConcern);
}

}  // namespace merizo
