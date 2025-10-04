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
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/global_catalog/type_shard_identity_gen.h"

#include <string>

namespace mongo {

/**
 * Contains all the information needed to make a mongod instance shard aware.
 */
class ShardIdentityType : public ShardIdentity {
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
