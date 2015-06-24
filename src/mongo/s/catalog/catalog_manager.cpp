/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/catalog_manager.h"

#include <memory>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_delete_document.h"
#include "mongo/s/write_ops/batched_delete_request.h"
#include "mongo/s/write_ops/batched_insert_request.h"
#include "mongo/s/write_ops/batched_update_document.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;

namespace {

Status getStatus(const BatchedCommandResponse& response) {
    if (response.getOk() == 0) {
        return Status(static_cast<ErrorCodes::Error>(response.getErrCode()),
                      response.getErrMessage());
    }

    if (response.isErrDetailsSet()) {
        const WriteErrorDetail* errDetail = response.getErrDetails().front();

        return Status(static_cast<ErrorCodes::Error>(errDetail->getErrCode()),
                      errDetail->getErrMessage());
    }

    if (response.isWriteConcernErrorSet()) {
        const WCErrorDetail* errDetail = response.getWriteConcernError();

        return Status(static_cast<ErrorCodes::Error>(errDetail->getErrCode()),
                      errDetail->getErrMessage());
    }

    return Status::OK();
}

}  // namespace

Status CatalogManager::insert(const string& ns,
                              const BSONObj& doc,
                              BatchedCommandResponse* response) {
    unique_ptr<BatchedInsertRequest> insert(new BatchedInsertRequest());
    insert->addToDocuments(doc);

    BatchedCommandRequest request(insert.release());
    request.setNS(ns);
    request.setWriteConcern(WriteConcernOptions::Majority);

    BatchedCommandResponse dummyResponse;
    if (response == NULL) {
        response = &dummyResponse;
    }

    // Make sure to add ids to the request, since this is an insert operation
    unique_ptr<BatchedCommandRequest> requestWithIds(BatchedCommandRequest::cloneWithIds(request));
    const BatchedCommandRequest& requestToSend = requestWithIds.get() ? *requestWithIds : request;

    writeConfigServerDirect(requestToSend, response);
    return getStatus(*response);
}

Status CatalogManager::update(const string& ns,
                              const BSONObj& query,
                              const BSONObj& update,
                              bool upsert,
                              bool multi,
                              BatchedCommandResponse* response) {
    unique_ptr<BatchedUpdateDocument> updateDoc(new BatchedUpdateDocument());
    updateDoc->setQuery(query);
    updateDoc->setUpdateExpr(update);
    updateDoc->setUpsert(upsert);
    updateDoc->setMulti(multi);

    unique_ptr<BatchedUpdateRequest> updateRequest(new BatchedUpdateRequest());
    updateRequest->addToUpdates(updateDoc.release());
    updateRequest->setWriteConcern(WriteConcernOptions::Majority);

    BatchedCommandRequest request(updateRequest.release());
    request.setNS(ns);

    BatchedCommandResponse dummyResponse;
    if (response == NULL) {
        response = &dummyResponse;
    }

    writeConfigServerDirect(request, response);
    return getStatus(*response);
}

Status CatalogManager::remove(const string& ns,
                              const BSONObj& query,
                              int limit,
                              BatchedCommandResponse* response) {
    unique_ptr<BatchedDeleteDocument> deleteDoc(new BatchedDeleteDocument);
    deleteDoc->setQuery(query);
    deleteDoc->setLimit(limit);

    unique_ptr<BatchedDeleteRequest> deleteRequest(new BatchedDeleteRequest());
    deleteRequest->addToDeletes(deleteDoc.release());
    deleteRequest->setWriteConcern(WriteConcernOptions::Majority);

    BatchedCommandRequest request(deleteRequest.release());
    request.setNS(ns);

    BatchedCommandResponse dummyResponse;
    if (response == NULL) {
        response = &dummyResponse;
    }

    writeConfigServerDirect(request, response);
    return getStatus(*response);
}

Status CatalogManager::updateCollection(const std::string& collNs, const CollectionType& coll) {
    fassert(28634, coll.validate());

    BatchedCommandResponse response;
    Status status = update(CollectionType::ConfigNS,
                           BSON(CollectionType::fullNs(collNs)),
                           coll.toBSON(),
                           true,   // upsert
                           false,  // multi
                           &response);
    if (!status.isOK()) {
        return Status(status.code(),
                      str::stream() << "collection metadata write failed: " << response.toBSON()
                                    << "; status: " << status.toString());
    }

    return Status::OK();
}

Status CatalogManager::updateDatabase(const std::string& dbName, const DatabaseType& db) {
    fassert(28616, db.validate());

    BatchedCommandResponse response;
    Status status = update(DatabaseType::ConfigNS,
                           BSON(DatabaseType::name(dbName)),
                           db.toBSON(),
                           true,   // upsert
                           false,  // multi
                           &response);
    if (!status.isOK()) {
        return Status(status.code(),
                      str::stream() << "database metadata write failed: " << response.toBSON()
                                    << "; status: " << status.toString());
    }

    return Status::OK();
}

// static
StatusWith<ShardId> CatalogManager::selectShardForNewDatabase(ShardRegistry* shardRegistry) {
    vector<ShardId> allShardIds;

    shardRegistry->getAllShardIds(&allShardIds);
    if (allShardIds.empty()) {
        shardRegistry->reload();
        shardRegistry->getAllShardIds(&allShardIds);

        if (allShardIds.empty()) {
            return Status(ErrorCodes::ShardNotFound, "No shards found");
        }
    }

    auto bestShard = shardRegistry->getShard(allShardIds[0]);
    if (!bestShard) {
        return {ErrorCodes::ShardNotFound, "Candidate shard disappeared"};
    }

    ShardStatus bestStatus = bestShard->getStatus();

    for (size_t i = 1; i < allShardIds.size(); i++) {
        const auto shard = shardRegistry->getShard(allShardIds[i]);
        if (!shard) {
            continue;
        }

        const ShardStatus status = shard->getStatus();

        if (status < bestStatus) {
            bestShard = shard;
            bestStatus = status;
        }
    }

    return bestShard->getId();
}

}  // namespace mongo
