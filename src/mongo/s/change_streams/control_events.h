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

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_id.h"

namespace mongo {

/**
 * Indicates that a chunk of a collection was moved from one shard to another.
 */
struct MoveChunkControlEvent {
    static constexpr auto opType = "moveChunk"_sd;

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
struct MovePrimaryControlEvent {
    static constexpr auto opType = "movePrimary"_sd;

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
struct NamespacePlacementChangedControlEvent {
    static constexpr auto opType = "namespacePlacementChanged"_sd;

    static NamespacePlacementChangedControlEvent createFromDocument(const Document& event);

    bool operator==(const NamespacePlacementChangedControlEvent& other) const = default;

    Timestamp clusterTime;
    Timestamp committedAt;
    NamespaceString nss;
};

/**
 * Indicates that a new database has been created. The control event corresponds to 'insert' into
 * 'config.databases' collection.
 */
struct DatabaseCreatedControlEvent {
    static constexpr auto opType = "insert"_sd;

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
ControlEvent parseControlEvent(const Document& changeEvent);
}  // namespace mongo
