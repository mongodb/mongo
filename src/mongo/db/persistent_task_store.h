/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <fmt/format.h>

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {

using namespace fmt::literals;

namespace WriteConcerns {

const WriteConcernOptions kMajorityWriteConcernShardingTimeout{
    WriteConcernOptions::kMajority,
    WriteConcernOptions::SyncMode::UNSET,
    WriteConcernOptions::kWriteConcernTimeoutSharding};

const WriteConcernOptions kMajorityWriteConcernNoTimeout{WriteConcernOptions::kMajority,
                                                         WriteConcernOptions::SyncMode::UNSET,
                                                         WriteConcernOptions::kNoTimeout};

const WriteConcernOptions kLocalWriteConcern;

}  // namespace WriteConcerns

template <typename T>
class PersistentTaskStore {
public:
    PersistentTaskStore(NamespaceString storageNss) : _storageNss(std::move(storageNss)) {}

    /**
     * Adds a task to the store.
     */
    void add(OperationContext* opCtx,
             const T& task,
             const WriteConcernOptions& writeConcern =
                 WriteConcerns::kMajorityWriteConcernShardingTimeout) {
        DBDirectClient dbClient(opCtx);

        const auto commandResponse = dbClient.runCommand([&] {
            write_ops::InsertCommandRequest insertOp(_storageNss);
            insertOp.setDocuments({task.toBSON()});
            return insertOp.serialize({});
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        WriteConcernResult ignoreResult;
        auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));
    }

    /**
     * Updates a document that matches the given query using the update modifier specified. Even if
     * multiple documents match, at most one document will be updated.
     */
    void update(OperationContext* opCtx,
                const BSONObj& filter,
                const BSONObj& update,
                const WriteConcernOptions& writeConcern =
                    WriteConcerns::kMajorityWriteConcernShardingTimeout) {
        _update(opCtx, filter, update, /* upsert */ false, writeConcern);
    }

    /**
     * Upserts a document that matches the given query using the update modifier specified. Even if
     * multiple documents match, at most one document will be updated.
     */
    void upsert(OperationContext* opCtx,
                const BSONObj& filter,
                const BSONObj& update,
                const WriteConcernOptions& writeConcern =
                    WriteConcerns::kMajorityWriteConcernShardingTimeout) {
        _update(opCtx, filter, update, /* upsert */ true, writeConcern);
    }

    /**
     * Removes all documents which match the given query.
     */
    void remove(OperationContext* opCtx,
                const BSONObj& filter,
                const WriteConcernOptions& writeConcern =
                    WriteConcerns::kMajorityWriteConcernShardingTimeout) {
        DBDirectClient dbClient(opCtx);

        auto commandResponse = dbClient.runCommand([&] {
            write_ops::DeleteCommandRequest deleteOp(_storageNss);

            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;

                entry.setQ(filter);
                entry.setMulti(true);

                return entry;
            }()});

            return deleteOp.serialize({});
        }());

        const auto commandReply = commandResponse->getCommandReply();
        uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

        WriteConcernResult ignoreResult;
        auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));
    }

    /**
     * Executes the specified query on the collection and calls the callback for each element.
     * Iteration can be stopped early if the callback returns false indicating that it doesn't want
     * to continue.
     */
    void forEach(OperationContext* opCtx,
                 const BSONObj& filter,
                 std::function<bool(const T&)> handler) {
        DBDirectClient dbClient(opCtx);

        FindCommandRequest findRequest{_storageNss};
        findRequest.setFilter(filter);
        auto cursor = dbClient.find(std::move(findRequest));

        while (cursor->more()) {
            auto bson = cursor->next();
            auto t =
                T::parse(IDLParserContext("PersistentTaskStore:" + _storageNss.toString()), bson);

            if (bool shouldContinue = handler(t); !shouldContinue)
                return;
        }
    }

    /**
     * Returns the number of documents in the store matching the given query.
     */
    size_t count(OperationContext* opCtx, const BSONObj& filter = BSONObj{}) {
        DBDirectClient client(opCtx);

        FindCommandRequest findRequest{_storageNss};
        findRequest.setFilter(filter);
        findRequest.setProjection(BSON("_id" << 1));
        auto cursor = client.find(std::move(findRequest));

        return cursor->itcount();
    }

private:
    void _update(OperationContext* opCtx,
                 const BSONObj& filter,
                 const BSONObj& update,
                 bool upsert,
                 const WriteConcernOptions& writeConcern =
                     WriteConcerns::kMajorityWriteConcernShardingTimeout) {
        DBDirectClient dbClient(opCtx);

        auto commandResponse = write_ops::checkWriteErrors(dbClient.update([&] {
            write_ops::UpdateCommandRequest updateOp(_storageNss);
            auto updateModification = write_ops::UpdateModification::parseFromClassicUpdate(update);
            write_ops::UpdateOpEntry updateEntry(filter, updateModification);
            updateEntry.setMulti(false);
            updateEntry.setUpsert(upsert);
            updateOp.setUpdates({updateEntry});
            return updateOp;
        }()));

        uassert(ErrorCodes::NoMatchingDocument,
                "No matching document found for query {} on namespace {}"_format(
                    filter.toString(), _storageNss.toString()),
                upsert || commandResponse.getN() > 0);

        WriteConcernResult ignoreResult;
        auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));
    }
    NamespaceString _storageNss;
};

}  // namespace mongo
