/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#pragma once

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/bitwise_enum_operators.h"

namespace mongo {

/**
 * Valid flags to pass to initializeStorageEngine. Used as a bitfield.
 */
enum StorageEngineInitFlags {
    kNone = 0,
    kAllowNoLockFile = 1 << 0,
};

/**
 * Initializes the storage engine on "service".
 */
void initializeStorageEngine(ServiceContext* service, StorageEngineInitFlags initFlags);

/**
 * Shuts down storage engine cleanly and releases any locks on mongod.lock.
 */
void shutdownGlobalStorageEngineCleanly(ServiceContext* service);

/**
 * Creates the lock file used to prevent concurrent processes from accessing the data files,
 * as appropriate.
 */
void createLockFile(ServiceContext* service);

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
    stdx::function<Status(const StorageEngine::Factory* const, const BSONObj&)> validateFunc);

/*
 * Appends a the list of available storage engines to a BSONObjBuilder for reporting purposes.
 */
void appendStorageEngineList(ServiceContext* service, BSONObjBuilder* result);

ENABLE_BITMASK_OPERATORS(StorageEngineInitFlags)

}  // namespace mongo
