// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace notify_sharding_event {

// List of supported ShardingEvent notification types
// TODO SERVER-100729 Remove any reference to the deprecated kDatabasesAdded event type
// once 9.0 becomes LTS.
[[MONGO_MOD_NEEDS_REPLACEMENT]] static constexpr char kDatabasesAdded[] = "databasesAdded";
[[MONGO_MOD_NEEDS_REPLACEMENT]] static constexpr char kCollectionSharded[] = "collectionSharded";
[[MONGO_MOD_NEEDS_REPLACEMENT]] static constexpr char kCollectionResharded[] =
    "collectionResharded";
[[MONGO_MOD_NEEDS_REPLACEMENT]] static constexpr char kNamespacePlacementChanged[] =
    "namespacePlacementChanged";
[[MONGO_MOD_NEEDS_REPLACEMENT]] static constexpr char kPlacementHistoryMetadataChanged[] =
    "placementHistoryMetadataChanged";

[[MONGO_MOD_NEEDS_REPLACEMENT]] inline Status validateEventType(const std::string& eventType) {
    if (eventType == kCollectionResharded || eventType == kNamespacePlacementChanged ||
        eventType == kDatabasesAdded || eventType == kCollectionSharded ||
        eventType == kPlacementHistoryMetadataChanged) {
        return Status::OK();
    }

    return {ErrorCodes::UnsupportedShardingEventNotification,
            "Unrecognized EventType: " + eventType};
}

}  // namespace notify_sharding_event
}  // namespace mongo
