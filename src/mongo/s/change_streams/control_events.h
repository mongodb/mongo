// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Indicates that a chunk of a collection was moved from one shard to another.
 */
struct [[MONGO_MOD_NEEDS_REPLACEMENT]] MoveChunkControlEvent {
    static constexpr auto opType = "moveChunk"sv;

    static MoveChunkControlEvent createFromDocument(const Document& event);

    bool operator==(const MoveChunkControlEvent& other) const = default;

    Timestamp clusterTime;
    ShardId fromShard;
    ShardId toShard;
    bool allCollectionChunksMigratedFromDonor;
};

/**
 * Indicates that a database got assigned a new primary shard.
 */
struct [[MONGO_MOD_PRIVATE]] MovePrimaryControlEvent {
    static constexpr auto opType = "movePrimary"sv;

    static MovePrimaryControlEvent createFromDocument(const Document& event);

    bool operator==(const MovePrimaryControlEvent& other) const = default;

    Timestamp clusterTime;
    ShardId fromShard;
    ShardId toShard;
};

/**
 * This event is an element in the general change stream reader notification mechanism which is used
 * to notify a change stream reader(s) about:
 * 1. changes in allocation of a collection/database to a set of shards;
 * 2. changes in permitted change stream reader operation mode - for example, that the change stream
 * v2 reader cannot operate anymore, and so on.
 */
struct [[MONGO_MOD_PRIVATE]] NamespacePlacementChangedControlEvent {
    static constexpr auto opType = "namespacePlacementChanged"sv;

    static NamespacePlacementChangedControlEvent createFromDocument(const Document& event);

    bool operator==(const NamespacePlacementChangedControlEvent& other) const = default;

    Timestamp clusterTime;
    NamespaceString nss;
};

/**
 * Indicates that a new database has been created. The control event corresponds to 'insert' into
 * 'config.databases' collection.
 */
struct [[MONGO_MOD_PRIVATE]] DatabaseCreatedControlEvent {
    static constexpr auto opType = "insert"sv;

    static DatabaseCreatedControlEvent createFromDocument(const Document& event);

    bool operator==(const DatabaseCreatedControlEvent& other) const = default;

    Timestamp clusterTime;
    DatabaseName createdDatabaseName;
};

using ControlEvent = std::variant<MoveChunkControlEvent,
                                  MovePrimaryControlEvent,
                                  NamespacePlacementChangedControlEvent,
                                  DatabaseCreatedControlEvent>;

/**
 * Parses the underlying 'changeEvent' and creates the right instance of the control event depending
 * on the 'DocumentSourceChangeStream::kOperationTypeField'. Throwns exception in case it was unable
 * to create the corresponding control event.
 */
[[MONGO_MOD_PRIVATE]] ControlEvent parseControlEvent(const Document& changeEvent);
}  // namespace mongo
