/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"

namespace mongo {
namespace notify_sharding_event {

// List of supported ShardingEvent notification types
// TODO SERVER-100729 Remove any reference to the deprecated kDatabasesAdded event type
// once 9.0 becomes LTS.
static constexpr char kDatabasesAdded[] = "databasesAdded";
static constexpr char kCollectionSharded[] = "collectionSharded";
static constexpr char kCollectionResharded[] = "collectionResharded";
static constexpr char kNamespacePlacementChanged[] = "namespacePlacementChanged";

inline Status validateEventType(const std::string& eventType) {
    if (eventType == kCollectionResharded || eventType == kNamespacePlacementChanged ||
        eventType == kDatabasesAdded || eventType == kCollectionSharded) {
        return Status::OK();
    }

    return {ErrorCodes::UnsupportedShardingEventNotification,
            "Unrecognized EventType: " + eventType};
}

}  // namespace notify_sharding_event
}  // namespace mongo
