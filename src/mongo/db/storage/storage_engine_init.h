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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/utility.h"

#include <functional>
#include <memory>
#include <vector>

namespace mongo {

/**
 * Valid flags to pass to initializeStorageEngine. Used as a bitfield.
 */
enum class StorageEngineInitFlags {
    kAllowNoLockFile = 1 << 0,
    kSkipMetadataFile = 1 << 1,
    kForRestart = 1 << 2,  // Used by reinitialzeStorageEngine only.
};

constexpr StorageEngineInitFlags operator&(StorageEngineInitFlags a, StorageEngineInitFlags b) {
    return StorageEngineInitFlags{stdx::to_underlying(a) & stdx::to_underlying(b)};
}

constexpr StorageEngineInitFlags operator|(StorageEngineInitFlags a, StorageEngineInitFlags b) {
    return StorageEngineInitFlags{stdx::to_underlying(a) | stdx::to_underlying(b)};
}

/**
 * Initializes the storage engine on "service".
 * The optional parameter `startupTimeElapsedBuilder` is for adding time elapsed of tasks done in
 * this function into one single builder that records the time elapsed during startup. Its default
 * value is nullptr because we only want to time this function when it is called during startup.
 */
StorageEngine::LastShutdownState initializeStorageEngine(
    OperationContext* opCtx,
    StorageEngineInitFlags initFlags,
    bool isReplSet,
    bool shouldRecoverFromOplogAsStandalone,
    bool inStandaloneMode,
    BSONObjBuilder* startupTimeElapsedBuilder = nullptr);

/**
 * Shuts down storage engine cleanly and releases any locks on mongod.lock.
 * Set `memLeakAllowed` to true for faster shutdown.
 */
void shutdownGlobalStorageEngineCleanly(ServiceContext* service, bool memLeakAllowed);

/**
 * Changes the storage engine for the given service by shutting down the old one and starting
 * up a new one.  Kills all opCtxs on the service context which have a storage recovery unit,
 * except the one passed in which has its recovery unit replaced.
 *
 * Changes to the configuration (e.g. to storageGlobalParams.dbpath) which need to happen while
 * no storage engine is active may be made in the changeConfigurationCallback.  At that point the
 * opCtx will have a no op recovery unit and any access to storage is not allowed.
 */
StorageEngine::LastShutdownState reinitializeStorageEngine(
    OperationContext* opCtx,
    StorageEngineInitFlags initFlags,
    bool isReplSet,
    bool shouldRecoverFromOplogAsStandalone,
    bool inStandaloneMode,
    std::function<void()> changeConfigurationCallback = [] {});

/**
 * Registers a storage engine onto the given "service".
 */
void registerStorageEngine(ServiceContext* service,
                           std::unique_ptr<StorageEngine::Factory> factory);

/**
 * Returns true if "name" refers to a registered storage engine.
 */
bool isRegisteredStorageEngine(ServiceContext* service, StringData name);

/**
 * Returns an unowned pointer to the factory for the named storage engine, or nullptr.
 *
 * NOTE: Exposed only for use in legacy testing scenarios.
 */
StorageEngine::Factory* getFactoryForStorageEngine(ServiceContext* context, StringData name);

/*
 * Extracts the storageEngine bson from the CollectionOptions provided.  Loops through each
 * provided storageEngine and asks the matching registered storage engine if the
 * collection/index options are valid.  Returns an error if the collection/index options are
 * invalid.
 * If no matching registered storage engine is found, return an error.
 * Validation function 'func' must be either:
 * - &StorageEngine::Factory::validateCollectionStorageOptions; or
 * - &StorageEngine::Factory::validateIndexStorageOptions
 */
Status validateStorageOptions(
    ServiceContext* service,
    const BSONObj& storageEngineOptions,
    std::function<Status(const StorageEngine::Factory* const, const BSONObj&)> validateFunc);

/**
 * Returns the list of all storage engines.
 */
std::vector<StringData> getStorageEngineNames(ServiceContext* svcCtx);

}  // namespace mongo
