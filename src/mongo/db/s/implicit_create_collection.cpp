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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/implicit_create_collection.h"

#include <map>
#include <memory>
#include <string>

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/create_collection_gen.h"

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {

/**
 * Responsible for explicitly creating collections in the sharding catalog. Also takes care of
 * making sure that concurrent attempts to create a collection for the same ns will be
 * synchronized and avoid duplicate work as much as possible.
 */

class CreateCollectionSerializer {
public:
    explicit CreateCollectionSerializer(NamespaceString ns) : _ns(std::move(ns)) {}

    /**
     * Initialize this collection so it will be officially tracked in a sharded environment
     * by sending the command to the config server to create an entry for this collection in
     * the sharding catalog.
     */
    Status onCannotImplicitlyCreateCollection(OperationContext* opCtx) noexcept {
        invariant(!opCtx->lockState()->isLocked());

        {
            stdx::unique_lock<stdx::mutex> lg(_mutex);
            while (_isInProgress) {
                auto status = opCtx->waitForConditionOrInterruptNoAssert(_cvIsInProgress, lg);
                if (!status.isOK()) {
                    return status;
                }
            }

            _isInProgress = true;
        }

        ON_BLOCK_EXIT([&] {
            stdx::lock_guard<stdx::mutex> lg(_mutex);
            _isInProgress = false;
            _cvIsInProgress.notify_one();
        });

        try {
            // Take the DBLock and CollectionLock directly rather than using AutoGetCollection
            // (which calls AutoGetDb) to avoid doing database and shard version checks.
            Lock::DBLock dbLock(opCtx, _ns.db(), MODE_IS);
            auto databaseHolder = DatabaseHolder::get(opCtx);
            auto db = databaseHolder->getDb(opCtx, _ns.db());
            if (db) {
                Lock::CollectionLock collLock(opCtx, _ns.ns(), MODE_IS);
                if (db->getCollection(opCtx, _ns.ns())) {
                    // Collection already created, no more work needs to be done.
                    return Status::OK();
                }
            }
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        ConfigsvrCreateCollection configCreateCmd(_ns);
        configCreateCmd.setDbName(NamespaceString::kAdminDb);

        auto statusWith =
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                NamespaceString::kAdminDb.toString(),
                CommandHelpers::appendMajorityWriteConcern(configCreateCmd.toBSON({})),
                Shard::RetryPolicy::kIdempotent);

        if (!statusWith.isOK()) {
            return statusWith.getStatus();
        }

        return Shard::CommandResponse::getEffectiveStatus(statusWith.getValue());
    }

private:
    const NamespaceString _ns;

    stdx::mutex _mutex;
    stdx::condition_variable _cvIsInProgress;
    bool _isInProgress = false;
};

class CreateCollectionSerializerMap {
public:
    std::shared_ptr<CreateCollectionSerializer> getForNs(const NamespaceString& ns) {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        auto iter = _inProgressMap.find(ns.ns());
        if (iter == _inProgressMap.end()) {
            std::tie(iter, std::ignore) =
                _inProgressMap.emplace(ns.ns(), std::make_shared<CreateCollectionSerializer>(ns));
        }

        return iter->second;
    }

    void cleanupNs(const NamespaceString& ns) {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        _inProgressMap.erase(ns.ns());
    }

private:
    stdx::mutex _mutex;
    std::map<std::string, std::shared_ptr<CreateCollectionSerializer>> _inProgressMap;
};

const auto createCollectionSerializerMap =
    ServiceContext::declareDecoration<CreateCollectionSerializerMap>();

}  // unnamed namespace

Status onCannotImplicitlyCreateCollection(OperationContext* opCtx,
                                          const NamespaceString& ns) noexcept {
    auto& handlerMap = createCollectionSerializerMap(opCtx->getServiceContext());
    auto status = handlerMap.getForNs(ns)->onCannotImplicitlyCreateCollection(opCtx);

    if (status.isOK()) {
        handlerMap.cleanupNs(ns);
    } else {
        // We only cleanup on success because that is our last chance for us to do so. This avoids
        // the scenario with multiple handlers for the same ns to exist at the same time if we
        // cleanup regardless of success or failure. On the other hand, cleaning it up only on
        // success can cause the handler to never get cleaned up when the collection was
        // successfully created, but this shard got an error response from the config
        // server.
    }

    return status;
}

}  // namespace mongo
