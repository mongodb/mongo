// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/global_catalog/type_shard_identity_gen.h"
#include "mongo/util/modules.h"

#include <string>

namespace mongo {

/**
 * Contains all the information needed to make a mongod instance shard aware.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardIdentityType : public ShardIdentity {
public:
    // The _id value for this document type.
    static const std::string IdName;

    ShardIdentityType() = default;
    ShardIdentityType(const ShardIdentity& sid) : ShardIdentity(sid) {}

    /**
     * Constructs a new ShardIdentityType object from a BSON object containing
     * a shard identity document. Also does validation of the contents.
     */
    static StatusWith<ShardIdentityType> fromShardIdentityDocument(const BSONObj& source);

    /**
     * Returns the BSON representation of the entry as a shard identity document.
     */
    BSONObj toShardIdentityDocument() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    /**
     * Returns OK if all fields have been set. Otherwise, returns NoSuchKey
     * and information about the first field that is missing.
     *
     * If fassert is true, validate will fassert if the server's cluster role matches the shard
     * identity document. This is intended to shutdown a server whose cluster role differs from the
     * primary and prevents a replica set from running with mixed cluster roles. See SERVER-80249
     * for more information.
     */
    Status validate(bool fassert = false) const;

    /**
     * Returns an update object that can be used to update the config server field of the
     * shardIdentity document with the new connection string.
     */
    static BSONObj createConfigServerUpdateObject(const std::string& newConnString);
};

}  // namespace mongo
