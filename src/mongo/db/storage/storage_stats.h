// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Manages statistics from the storage engine, allowing addition of statistics, serialization to
 * BSON, and access to certain metrics.
 */
class [[MONGO_MOD_OPEN]] StorageStats {
public:
    virtual ~StorageStats() = default;

    virtual void appendToBsonObjBuilder(BSONObjBuilder& builder) const = 0;
    virtual BSONObj toBSON() const = 0;

    virtual uint64_t bytesRead() const = 0;
    virtual Microseconds readingTime() const = 0;

    virtual std::unique_ptr<StorageStats> clone() const = 0;

    virtual StorageStats& operator+=(const StorageStats&) = 0;
    virtual StorageStats& operator-=(const StorageStats&) = 0;
};

}  // namespace mongo
