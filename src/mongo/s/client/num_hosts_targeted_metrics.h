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

#include <memory>
#include <vector>

#include "mongo/platform/atomic_word.h"


namespace mongo {

class BSONObjBuilder;
class OperationContext;
class ServiceContext;

struct TargetStats {
    // the op targeted all shards that own chunks for the collection as long as the total number of
    // shards in the cluster is > 1
    AtomicWord<int> allShards;

    // the op targeted > 1 shard that own chunks for the collection as long as the total number of
    // shards in the cluster is > 1
    AtomicWord<int> manyShards;

    // the op targeted 1 shard (if there exists only one shard, the metric will count as 'oneShard')
    AtomicWord<int> oneShard;

    // the collection is unsharded
    AtomicWord<int> unsharded;
};

class NumHostsTargetedMetrics {

public:
    enum QueryType {
        kFindCmd,
        kInsertCmd,
        kUpdateCmd,
        kDeleteCmd,
        kAggregateCmd,
        kNumQueryType,
    };

    enum TargetType { kAllShards, kManyShards, kOneShard, kUnsharded };

    void addNumHostsTargeted(QueryType queryType, TargetType targetType);

    void appendSection(BSONObjBuilder* builder);

    TargetType parseTargetType(OperationContext* opCtx,
                               int nShardsTargeted,
                               int nShardsOwningChunks);

    static NumHostsTargetedMetrics& get(ServiceContext* serviceContext);
    static NumHostsTargetedMetrics& get(OperationContext* opCtx);

    void startup();

private:
    std::vector<std::unique_ptr<TargetStats>> _numHostsTargeted;
};

}  // namespace mongo
