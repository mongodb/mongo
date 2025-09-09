/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/rss/attached_storage/attached_persistence_provider.h"

#include "mongo/base/string_data.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"

namespace mongo::rss {
namespace {
// Checkpoint every 60 seconds by default.
constexpr double kDefaultAttachedSyncDelaySeconds = 60.0;

ServiceContext::ConstructorActionRegisterer registerAttachedPersistenceProvider{
    "AttachedPersistenceProvider", [](ServiceContext* service) {
        auto& rss = ReplicatedStorageService::get(service);
        rss.setPersistenceProvider(std::make_unique<AttachedPersistenceProvider>());
    }};

}  // namespace

std::string AttachedPersistenceProvider::name() const {
    return "Attached Storage";
}

boost::optional<Timestamp> AttachedPersistenceProvider::getSentinelDataTimestamp() const {
    return boost::none;
}

std::string AttachedPersistenceProvider::getWiredTigerConfig(int) const {
    return "";
}

bool AttachedPersistenceProvider::shouldUseReplicatedCatalogIdentifiers() const {
    return false;
}

bool AttachedPersistenceProvider::shouldUseReplicatedRecordIds() const {
    return false;
}

bool AttachedPersistenceProvider::shouldUseOplogWritesForFlowControlSampling() const {
    return true;
}

bool AttachedPersistenceProvider::shouldStepDownForShutdown() const {
    return true;
}

bool AttachedPersistenceProvider::shouldDelayDataAccessDuringStartup() const {
    return false;
}

bool AttachedPersistenceProvider::shouldAvoidDuplicateCheckpoints() const {
    return false;
}

bool AttachedPersistenceProvider::supportsLocalCollections() const {
    return true;
}

bool AttachedPersistenceProvider::supportsUnstableCheckpoints() const {
    return true;
}

bool AttachedPersistenceProvider::supportsTableLogging() const {
    return true;
}

bool AttachedPersistenceProvider::supportsCrossShardTransactions() const {
    return true;
}

}  // namespace mongo::rss
