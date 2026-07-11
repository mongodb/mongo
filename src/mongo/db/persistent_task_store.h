// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

#include <fmt/format.h>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

template <typename T>
class PersistentTaskStore {
public:
    PersistentTaskStore(NamespaceString storageNss) : _storageNss(std::move(storageNss)) {}

    /**
     * Adds a task to the store.
     */
    void add(OperationContext* opCtx,
             const T& task,
             const WriteConcernOptions& writeConcern = defaultMajorityWriteConcernDoNotUse()) {
        DBDirectClient dbClient(opCtx);

        const auto commandResponse = dbClient.runCommand([&] {
            write_ops::InsertCommandRequest insertOp(_storageNss);
            insertOp.setDocuments({task.toBSON()});
            return insertOp.serialize();
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
                const WriteConcernOptions& writeConcern = defaultMajorityWriteConcernDoNotUse()) {
        _update(opCtx, filter, update, /* upsert */ false, writeConcern);
    }

    /**
     * Upserts a document that matches the given query using the update modifier specified. Even if
     * multiple documents match, at most one document will be updated.
     */
    void upsert(OperationContext* opCtx,
                const BSONObj& filter,
                const BSONObj& update,
                const WriteConcernOptions& writeConcern = defaultMajorityWriteConcernDoNotUse()) {
        _update(opCtx, filter, update, /* upsert */ true, writeConcern);
    }

    /**
     * Removes all documents which match the given query.
     */
    void remove(OperationContext* opCtx,
                const BSONObj& filter,
                const WriteConcernOptions& writeConcern = defaultMajorityWriteConcernDoNotUse()) {
        DBDirectClient dbClient(opCtx);

        auto commandResponse = dbClient.runCommand([&] {
            write_ops::DeleteCommandRequest deleteOp(_storageNss);

            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;

                entry.setQ(filter);
                entry.setMulti(true);

                return entry;
            }()});

            return deleteOp.serialize();
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
            auto t = T::parse(
                bson, IDLParserContext("PersistentTaskStore:" + _storageNss.toStringForErrorMsg()));

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
                 const WriteConcernOptions& writeConcern = defaultMajorityWriteConcernDoNotUse()) {
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
                fmt::format("No matching document found for query {} on namespace {}",
                            filter.toString(),
                            _storageNss.toStringForErrorMsg()),
                upsert || commandResponse.getN() > 0);

        WriteConcernResult ignoreResult;
        auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
        uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, writeConcern, &ignoreResult));
    }

    NamespaceString _storageNss;
};

}  // namespace mongo
